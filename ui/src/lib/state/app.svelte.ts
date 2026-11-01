import { createLogger } from '../log'
import type { QueryFilter } from '../engine/analyses/basic'
import type { Baseline } from '../engine/analyses/diff'
import type { RetainerFilter } from '../engine/analyses/filters'
import { DumpClient, loadBaseline, loadHistograms, WorkerFailure } from '../worker/client'
import type { HistogramTrend } from '../engine/analyses/histogram'
import type { Analysis, Overview, Phase } from '../worker/protocol'

/**
 * Application state.
 *
 * Every value that comes from the worker is held in $state.raw: replaced, never
 * mutated, never wrapped in proxies. The dump itself never reaches the page —
 * only the DumpClient handle, which is not reactive state at all.
 */

const log = createLogger('app')

/** Chrome holds about 15M items (~2.5 GB of dump) within a 2 GB worker budget. */
export const LARGE_DUMP_BYTES = 2.5 * 1024 ** 3

const LOADING_MARKER = 'meminfo-ui:loading'

export type View = 'suspects' | 'findings' | 'growth' | 'summary' | 'top' | 'search' | 'graph'

export interface LoadProgress {
  phase: Phase
  bytes: number
  totalBytes: number
  items: number
}

export interface LoadError {
  kind: string
  message: string
  fileName: string
}

/** What a load that crashed the tab left behind in sessionStorage. */
export interface CrashReport {
  fileName: string
  fileSize: number
  phase: string
}

type Status = 'empty' | 'loading' | 'ready' | 'error' | 'crashed'

let client: DumpClient | null = null
let baselineAbort: (() => void) | null = null
/**
 * Baselines of a series whose newest dump waits for "Load anyway" (it is
 * large): kept with that dump only, and dropped when another file is opened.
 */
let pendingLargeBaselines: File[] = []

function readMarker(): CrashReport | null {
  try {
    const text = sessionStorage.getItem(LOADING_MARKER)
    return text === null ? null : (JSON.parse(text) as CrashReport)
  } catch {
    return null
  }
}

function writeMarker(report: CrashReport | null): void {
  try {
    if (report === null) sessionStorage.removeItem(LOADING_MARKER)
    else sessionStorage.setItem(LOADING_MARKER, JSON.stringify(report))
  } catch {
    // Storage unavailable: crash detection degrades to the worker's own death.
  }
}

/**
 * Retainer filters live in the URL hash (#ignore=…), so a shared link shows the
 * same view of the same dump. The dump itself is never in the URL.
 */
function readHashFilters(): RetainerFilter[] {
  try {
    const value = new URLSearchParams(globalThis.location?.hash.slice(1) ?? '').get('ignore')
    if (value === null) return []
    const parsed: unknown = JSON.parse(value)
    return Array.isArray(parsed) ? (parsed as RetainerFilter[]) : []
  } catch {
    return []
  }
}

function writeHashFilters(filters: RetainerFilter[]): void {
  try {
    const hash = filters.length === 0 ? '' : `#ignore=${encodeURIComponent(JSON.stringify(filters))}`
    history.replaceState(null, '', `${location.pathname}${location.search}${hash}`)
  } catch {
    // No history API (tests): the filters still apply, only the link does not keep them.
  }
}

class AppState {
  status = $state<Status>('empty')
  isDemo = $state(false)
  view = $state<View>('suspects')
  overview = $state.raw<Overview | null>(null)
  analysis = $state.raw<Analysis | null>(null)
  progress = $state.raw<LoadProgress | null>(null)
  error = $state.raw<LoadError | null>(null)
  crash = $state.raw<CrashReport | null>(null)
  pendingLarge = $state.raw<File | null>(null)

  /** Earlier dumps of the same process, oldest first (by file modification time). */
  baselines = $state.raw<Array<Baseline & { modified: number }>>([])
  baselineProgress = $state.raw<LoadProgress | null>(null)
  baselineError = $state<string | null>(null)

  /** Filters the search view starts from (set when another view links to it). */
  searchFilters = $state.raw<QueryFilter[]>([])

  /** Item the graph view is centred on. */
  graphTarget = $state<number | null>(null)

  /** Item shown in the inspector, and the way back. */
  selected = $state<number | null>(null)
  history = $state.raw<number[]>([])

  /** Bumped when a new dump is ready, so views refetch. */
  generation = $state(0)

  /** Class counts over time from meminfo_histogram() files; independent of the open dump. */
  histograms = $state.raw<HistogramTrend | null>(null)
  histogramsError = $state<string | null>(null)

  /** Holders ignored when computing what keeps items alive (engine/analyses/filters). */
  retainerFilters = $state.raw<RetainerFilter[]>([])
  filtersError = $state<string | null>(null)

  constructor() {
    // A marker left by a load that never finished: the tab most likely crashed
    // (a worker out of JS heap takes the whole tab down in Chromium).
    const marker = readMarker()
    if (marker !== null) {
      this.crash = marker
      this.status = 'crashed'
      writeMarker(null)
      log.error('the previous load did not finish', { ...marker })
    }
  }

  get client(): DumpClient | null {
    return client
  }

  /** Entry point for a picked or dropped file. */
  open(file: File, force = false, baselines: File[] = []): void {
    // "Load anyway" on a large newest dump of a series keeps that series.
    const series = force && file === this.pendingLarge ? pendingLargeBaselines : baselines
    pendingLargeBaselines = []
    if (!force && file.size > LARGE_DUMP_BYTES) {
      this.pendingLarge = file
      pendingLargeBaselines = baselines
      return
    }
    this.pendingLarge = null
    void this.load(file, series)
  }

  /** The baselines of a series belong to this load only: a failed or cancelled one drops them. */
  private async load(file: File, baselines: File[] = []): Promise<void> {
    this.close()
    this.status = 'loading'
    this.error = null
    this.crash = null
    this.progress = { phase: 'reading', bytes: 0, totalBytes: file.size, items: 0 }
    const started = performance.now()
    log.info('load started', { size: file.size })

    const own = new DumpClient()
    client = own
    own.onDeath((failure) => this.workerDied(file, failure))
    writeMarker({ fileName: file.name, fileSize: file.size, phase: 'reading' })

    try {
      const overview = await own.load(file, (phase, bytes, totalBytes, items) => {
        this.progress = { phase, bytes, totalBytes, items }
      })
      if (client !== own) return
      this.overview = overview
      writeMarker({ fileName: file.name, fileSize: file.size, phase: 'analyzing' })
      this.progress = { phase: 'analyzing', bytes: file.size, totalBytes: file.size, items: overview.stats.items }

      const { analysis, filters } = await this.analyzeWithLinkFilters(own)
      if (client !== own) return
      this.analysis = analysis
      this.retainerFilters = filters
      this.status = 'ready'
      this.view = 'suspects'
      this.retainersFor = null
      this.generation++
      this.progress = null
      writeMarker(null)
      log.info('load finished', { items: overview.stats.items, ms: Math.round(performance.now() - started) })
      for (const baselineFile of baselines) {
        if (client !== own) return
        await this.openBaseline(baselineFile)
      }
    } catch (error) {
      if (client !== own) return
      writeMarker(null)
      const failure = error instanceof WorkerFailure ? error : new WorkerFailure({ kind: 'internal', message: String(error) })
      if (failure.kind === 'cancelled') {
        log.warn('load cancelled')
        this.status = 'empty'
        return
      }
      if (failure.kind === 'worker-died') return // handled by workerDied
      this.isDemo = false
      this.error = { kind: failure.kind, message: failure.message, fileName: file.name }
      this.status = 'error'
      this.dropClient()
    }
  }

  /**
   * Analyze with the retainer filters of the link (#ignore=…). A link whose
   * filters cannot be applied — an invalid pattern, edited or cut short by a
   * messenger — must not keep the dump from opening: it is analyzed without
   * them, the error is shown and the link is cleaned.
   */
  private async analyzeWithLinkFilters(own: DumpClient): Promise<{ analysis: Analysis; filters: RetainerFilter[] }> {
    const fromLink = readHashFilters()
    if (fromLink.length === 0) {
      return { analysis: await own.request({ kind: 'analyze' }), filters: [] }
    }
    try {
      return { analysis: await own.request({ kind: 'setFilters', filters: fromLink }), filters: fromLink }
    } catch (error) {
      if (!(error instanceof WorkerFailure) || error.kind !== 'invalid-query') throw error
      log.warn('the retainer filters of the link were ignored', { filters: fromLink.length, error: error.message })
      this.filtersError = `The filters of the link were ignored: ${error.message}`
      writeHashFilters([])
      return { analysis: await own.request({ kind: 'analyze' }), filters: [] }
    }
  }

  private workerDied(file: File, failure: WorkerFailure): void {
    this.isDemo = false
    log.error('analysis worker died', { size: file.size, phase: this.progress?.phase ?? 'ready', message: failure.message })
    writeMarker(null)
    this.crash = { fileName: file.name, fileSize: file.size, phase: this.progress?.phase ?? 'browsing' }
    this.status = 'crashed'
    client = null
    this.resetDumpState()
  }

  cancel(): void {
    log.warn('load cancelled by the user')
    writeMarker(null)
    this.close()
    this.status = 'empty'
  }

  /** Drop the dump: terminating the worker frees everything it held. */
  close(): void {
    this.dropClient()
    this.resetDumpState()
    this.status = 'empty'
  }

  dismiss(): void {
    this.crash = null
    this.error = null
    this.status = 'empty'
  }

  private dropClient(): void {
    client?.dispose()
    client = null
  }

  private resetDumpState(): void {
    this.isDemo = false
    baselineAbort?.()
    this.overview = null
    this.analysis = null
    this.progress = null
    this.baselines = []
    this.baselineProgress = null
    this.baselineError = null
    this.selected = null
    this.history = []
    this.searchFilters = []
    this.retainerFilters = []
    this.filtersError = null
    // Indices mean nothing in another dump: a graph must not outlive its dump.
    this.graphTarget = null
  }

  /** The most recent baseline: what the two-dump comparison is against. */
  get baseline(): Baseline | null {
    return this.baselines.at(-1) ?? null
  }

  /**
   * Several dumps at once (a series from dump-meminfo --count): the newest is
   * opened, the others become its baselines.
   */
  openSeries(files: File[]): void {
    if (files.length === 1) {
      this.open(files[0]!)
      return
    }
    const sorted = [...files].sort((a, b) => a.lastModified - b.lastModified)
    log.info('series opened', { dumps: files.length })
    this.open(sorted.at(-1)!, false, sorted.slice(0, -1))
  }

  async openBaseline(file: File): Promise<void> {
    baselineAbort?.()
    this.baselineError = null
    this.baselineProgress = { phase: 'reading', bytes: 0, totalBytes: file.size, items: 0 }
    const controller = new AbortController()
    baselineAbort = () => controller.abort()
    const aborted = (): boolean => controller.signal.aborted
    try {
      const baseline = await loadBaseline(
        file,
        (phase, bytes, totalBytes, items) => {
          if (!aborted()) this.baselineProgress = { phase, bytes, totalBytes, items }
        },
        controller.signal,
      )
      if (aborted()) return
      this.baselines = [...this.baselines, { ...baseline, modified: file.lastModified }].sort((a, b) => a.modified - b.modified)
      this.view = 'growth'
      log.info('baseline loaded', { items: baseline.items })
    } catch (error) {
      if (!aborted()) this.baselineError = error instanceof Error ? error.message : String(error)
    } finally {
      if (!aborted()) this.baselineProgress = null
    }
  }

  clearBaseline(): void {
    baselineAbort?.()
    this.baselines = []
    this.baselineProgress = null
    this.baselineError = null
  }

  async openHistograms(files: File[]): Promise<void> {
    log.info('histograms opened', { files: files.length })
    try {
      this.histograms = await loadHistograms(files)
      this.histogramsError = null
      this.view = 'growth'
    } catch (error) {
      this.histogramsError = error instanceof Error ? error.message : String(error)
    }
  }

  removeBaseline(at: number): void {
    this.baselines = this.baselines.filter((_, k) => k !== at)
  }

  /** Open the graph view on an item. */
  openGraph(index: number): void {
    log.debug('graph opened', { index })
    this.graphTarget = index
    this.view = 'graph'
  }

  /** Class or type whose instances the suspects view groups by retainer path. */
  retainersFor = $state.raw<{ key: string; isClass: boolean } | null>(null)

  /** Show how the instances of a class or type are held (null closes the panel). */
  showRetainers(target: { key: string; isClass: boolean } | null): void {
    log.debug('retainers opened', { key: target?.key })
    this.retainersFor = target
    if (target !== null) this.view = 'suspects'
  }

  /** Recompute reachability without the given holders; views refetch. */
  async setRetainerFilters(filters: RetainerFilter[]): Promise<void> {
    const own = client
    if (own === null) return
    log.info('retainer filters', { filters: filters.length })
    try {
      const analysis = await own.request({ kind: 'setFilters', filters })
      if (client !== own) return
      this.analysis = analysis
      this.retainerFilters = filters
      this.filtersError = null
      writeHashFilters(filters)
      this.generation++
    } catch (error) {
      this.filtersError = error instanceof Error ? error.message : String(error)
      log.warn('retainer filters failed', { error: this.filtersError })
    }
  }

  /** Ignore every reference from items of this class or type. */
  ignoreHolder(kind: string): void {
    const pattern = `^${kind.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`
    if (this.retainerFilters.some((filter) => filter.kind === 'holders' && filter.pattern === pattern)) return
    void this.setRetainerFilters([...this.retainerFilters, { kind: 'holders', pattern }])
  }

  /** Open the search view on the instances of a type or class. */
  searchFor(key: string, isClass: boolean): void {
    this.searchFilters = [{ attribute: isClass ? 'class' : 'type', operator: '=', value: key }]
    this.view = 'search'
  }

  select(index: number | null): void {
    if (index === this.selected) return
    if (this.selected !== null && index !== null) this.history = [...this.history.slice(-49), this.selected]
    if (index === null) this.history = []
    this.selected = index
  }

  back(): void {
    const previous = this.history.at(-1)
    if (previous === undefined) return
    this.history = this.history.slice(0, -1)
    this.selected = previous
  }
}

export const app = new AppState()
