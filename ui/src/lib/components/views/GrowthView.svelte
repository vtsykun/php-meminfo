<script lang="ts">
  import type { GrowthRow, SeriesResult } from '../../engine/analyses/diff'
  import type { LeaksResult } from '../../worker/protocol'
  import type { GenerationTable } from '../../engine/analyses/generations'
  import { takenMidWork } from '../../engine/store'
  import ItemLabel from '../ItemLabel.svelte'
  import { formatBytes, formatCount, formatDelta } from '../../format'
  import { app } from '../../state/app.svelte'
  import Progress from '../Progress.svelte'
  import VirtualTable, { type Column } from '../VirtualTable.svelte'

  let input: HTMLInputElement | undefined = $state()
  let histogramInput: HTMLInputElement | undefined = $state()

  function pickHistograms(event: Event): void {
    const target = event.currentTarget as HTMLInputElement
    const files = [...(target.files ?? [])]
    target.value = ''
    if (files.length > 0) void app.openHistograms(files)
  }
  let rows = $state.raw<GrowthRow[] | null>(null)
  let failed = $state<string | null>(null)
  let changedOnly = $state(true)
  let metric = $state<'sizeDelta' | 'countDelta'>('sizeDelta')

  $effect(() => {
    void app.generation
    const baseline = app.baseline
    const client = app.client
    rows = null
    failed = null
    if (client === null || baseline === null) return
    let cancelled = false
    client
      .request({ kind: 'diff', baseline })
      .then((value) => {
        if (!cancelled) rows = value
      })
      .catch((error: unknown) => {
        if (!cancelled) failed = String(error)
      })
    return () => {
      cancelled = true
    }
  })

  // meminfo_mark() generations of this dump, when it has them.
  let generations = $state.raw<GenerationTable | null>(null)
  // Show the objects that survived since this generation: created at it or before.
  let survivedSince = $state<number | null>(null)

  const survivorRows = $derived.by(() => {
    if (generations === null) return []
    const table = generations
    const since = survivedSince ?? table.generations.at(-2) ?? table.generations[0]!
    return table.rows
      .map((row) => ({
        ...row,
        survived: table.generations.reduce((sum, g, at) => (g <= since ? sum + row.counts[at]! : sum), 0),
        fresh: row.counts.at(-1)!,
      }))
      .filter((row) => row.survived > 0)
      .sort((a, b) => b.survived - a.survived || a.key.localeCompare(b.key))
  })

  $effect(() => {
    void app.generation
    const client = app.client
    generations = null
    if (client === null) return
    let cancelled = false
    client
      .request({ kind: 'generations' })
      .then((value) => {
        if (!cancelled) generations = value
      })
      .catch(() => {
        // A dump without generations has nothing to show here.
      })
    return () => {
      cancelled = true
    }
  })

  // Three dumps or more: what grows at every step, and what the work between
  // the first two left behind.
  let series = $state.raw<SeriesResult | null>(null)
  let leaks = $state.raw<LeaksResult | null>(null)
  let seriesFailed = $state<string | null>(null)

  $effect(() => {
    void app.generation
    const baselines = app.baselines
    const client = app.client
    series = null
    leaks = null
    seriesFailed = null
    if (client === null || baselines.length < 2) return
    let cancelled = false
    Promise.all([
      client.request({ kind: 'series', baselines }),
      client.request({ kind: 'leaks', first: baselines[0]!, second: baselines.at(-1)! }),
    ])
      .then(([seriesResult, leaksResult]) => {
        if (cancelled) return
        series = seriesResult
        leaks = leaksResult
      })
      .catch((error: unknown) => {
        if (!cancelled) seriesFailed = String(error)
      })
    return () => {
      cancelled = true
    }
  })

  // Dumps taken wherever the code happened to be hold the temporaries of the
  // job then running: what looks like growth may be one job's working set.
  const midWork = $derived(
    [
      ...app.baselines.filter((baseline) => baseline.header !== undefined && takenMidWork(baseline.header)).map((baseline) => baseline.name),
      ...(app.overview && takenMidWork(app.overview.header) ? [`${app.overview.fileName} (current)`] : []),
    ],
  )

  const incompatible = $derived.by(() => {
    const current = app.overview?.header
    if (!current) return false
    return app.baselines.some(({ header }) => header && (
      (header.pid !== undefined && current.pid !== undefined && header.pid !== current.pid) ||
      (header.size_model ?? 1) !== (current.size_model ?? 1) ||
      (header.identity_model ?? 1) !== (current.identity_model ?? 1)
    ))
  })

  const shown = $derived(rows === null ? null : changedOnly ? rows.filter((row) => row.countDelta !== 0 || row.sizeDelta !== 0) : rows)

  function pick(event: Event): void {
    const target = event.currentTarget as HTMLInputElement
    const file = target.files?.[0]
    target.value = ''
    if (file !== undefined) void app.openBaseline(file)
  }

  const columns: Column<GrowthRow>[] = [
    { key: 'key', label: 'Type or class', width: 'minmax(180px, 1fr)', value: (row) => row.key },
    { key: 'before', label: 'Before', width: '100px', align: 'right', value: (row) => formatCount(row.before.count), compare: (a, b) => a.before.count - b.before.count },
    { key: 'after', label: 'Now', width: '100px', align: 'right', value: (row) => formatCount(row.after.count), compare: (a, b) => a.after.count - b.after.count },
    { key: 'countDelta', label: 'Δ count', width: '100px', align: 'right', value: (row) => formatDelta(row.countDelta), compare: (a, b) => a.countDelta - b.countDelta },
    { key: 'sizeDelta', label: 'Δ self size', width: '110px', align: 'right', value: (row) => formatDelta(row.sizeDelta, formatBytes), compare: (a, b) => a.sizeDelta - b.sizeDelta },
  ]
</script>

<section class="view">
  <header class="head">
    <div>
      <h2>Growth</h2>
      <p class="sub">
        Compare equivalent points in the same process. Growth is a clue to investigate, not proof of a leak.
      </p>
    </div>
    {#if app.baselines.length > 0}
      <div class="baseline">
        <span>
          {app.baselines.length === 1 ? 'Baseline' : 'Baselines, oldest first:'}
          {#each app.baselines as baseline, at (at)}
            <strong>{baseline.name}</strong>
            <button type="button" class="button quiet" aria-label="Remove {baseline.name}" onclick={() => app.removeBaseline(at)}>✕</button>
          {/each}
        </span>
        <button type="button" class="button quiet" onclick={() => input?.click()}>Add another</button>
        <button type="button" class="button quiet" onclick={() => app.clearBaseline()}>Remove all</button>
      </div>
    {/if}
    <input bind:this={histogramInput} class="visually-hidden" type="file" multiple accept=".json,application/json" onchange={pickHistograms} aria-label="Histogram files" />
    <button type="button" class="button quiet" onclick={() => histogramInput?.click()}>Open histograms…</button>
    <input bind:this={input} class="visually-hidden" type="file" accept=".json,application/json" onchange={pick} aria-label="Baseline dump" />
  </header>

  {#if incompatible}
    <p class="notice warning">These dumps have different process IDs or memory models. Their measurements may not be comparable.</p>
  {/if}
  {#if app.baselines.length > 0 && midWork.length > 0}
    <p class="notice">
      Taken in the middle of the work ({midWork.join(', ')}): these dumps hold the temporaries of the job running at the
      time, which can show up as growth. Dumps taken between jobs (<code>meminfo.dump_defer</code> and
      <code>meminfo_dump_if_requested()</code>) compare more cleanly; a dump on <code>memory_limit</code> cannot wait for
      that.
    </p>
  {/if}

  {#if app.histogramsError}
    <div class="notice danger" role="alert">{app.histogramsError}</div>
  {/if}
  {#if app.histograms}
    <div class="block">
      <h3>Class counts over {app.histograms.names.length} histograms (meminfo_histogram())</h3>
      <p class="hint">Classes whose count went up at every sample come first.</p>
      <ul class="list">
        {#each app.histograms.rows.slice(0, 25) as row (row.key)}
          <li>
            <span class:grew={row.growing}>{row.key}</span>
            <span class="sample-bars" aria-hidden="true">{#each row.counts as count}<span style:height="{Math.max(3, count / row.counts.reduce((max, value) => Math.max(max, value), 1) * 32)}px"></span>{/each}</span>
              <span class="num muted">{row.counts.map((count) => formatCount(count)).join(' → ')}</span>
          </li>
        {/each}
      </ul>
    </div>
  {/if}

  {#if generations && generations.rows.length > 0}
    <div class="block">
      <h3>Objects by generation (meminfo_mark())</h3>
      <p class="hint">
        How many instances of each class were first seen at each mark. Old generations that stay populated are objects
        kept after the work that created them; the last column is what was created since the last mark.
      </p>
      {#if generations.generations.length > 1}
        <label class="toggle">
          Survived since generation
          <select
            value={survivedSince ?? generations.generations.at(-2)}
            onchange={(event) => (survivedSince = Number(event.currentTarget.value))}
          >
            {#each generations.generations.slice(0, -1) as g (g)}<option value={g}>#{g}</option>{/each}
          </select>
        </label>
      {/if}
      <table class="generations">
        <thead>
          <tr>
            <th>Class</th>
            {#each generations.generations as g (g)}<th class="num">#{g}</th>{/each}
            <th class="num" title="Instances from the chosen generation or older, still alive">Survived</th>
            <th class="num" title="Instances created since the last mark">New</th>
          </tr>
        </thead>
        <tbody>
          {#each survivorRows.slice(0, 20) as row (row.key)}
            <tr>
              <td><button type="button" class="link" onclick={() => app.searchFor(row.key, true)}>{row.key}</button></td>
              {#each row.counts as count, at (at)}<td class="num">{count === 0 ? '' : formatCount(count)}</td>{/each}
              <td class="num"><strong>{formatCount(row.survived)}</strong></td>
              <td class="num">{formatCount(row.fresh)}</td>
            </tr>
          {:else}
            <tr><td colspan={generations.generations.length + 3}>No object of that generation or older is alive.</td></tr>
          {/each}
        </tbody>
      </table>
    </div>
  {/if}

  {#if app.baselineProgress}
    <Progress title="Reading the baseline" progress={app.baselineProgress} onCancel={() => app.clearBaseline()} />
  {:else if app.baselineError}
    <div class="notice danger" role="alert">The baseline could not be read: {app.baselineError}</div>
  {:else if app.baseline === null}
    <div class="empty">
      <p>
        Load the dump taken <em>earlier</em> as a baseline. Only its totals per class are kept, so comparing does not need
        memory for a second dump. With two baselines or more (or several files opened at once, from
        <code>dump-meminfo --count=3</code>), objects created between the first two and still alive are listed as leak
        candidates, and what grows at every step is shown.
      </p>
      <button type="button" class="button primary" onclick={() => input?.click()}>Choose baseline dump…</button>
    </div>
  {:else if failed}
    <div class="notice danger" role="alert">{failed}</div>
  {:else if shown === null}
    <div class="skeleton" style:height="240px"></div>
  {:else}
    {#if seriesFailed}
      <div class="notice danger" role="alert">{seriesFailed}</div>
    {/if}
    {#if leaks}
      <div class="block">
        <h3>Leak candidates: {formatCount(leaks.candidates)} objects</h3>
        <p class="hint">
          Created after <strong>{app.baselines[0]?.name}</strong>, present in <strong>{app.baselines.at(-1)?.name}</strong> and
          still alive now. Objects are matched by handle and class; handles can be reused, so these are candidates, not confirmed survivors.
        </p>
        {#if leaks.candidates > 0}
          <ul class="list">
            {#each leaks.byClass.slice(0, 10) as row (row.key)}
              <li>
                <button type="button" class="link" onclick={() => app.searchFor(row.key, true)}>{row.key}</button>
                <span class="num">{formatCount(row.count)}</span>
                <span class="num muted" title="Sum of retained sizes may overlap">{formatBytes(row.retained)} (overlapping sum)</span>
              </li>
            {/each}
          </ul>
          <h4>Where they are held</h4>
          <ol class="list">
            {#each leaks.retainers.clusters.slice(0, 8) as cluster, at (at)}
              <li>
                <span class="num">{formatCount(cluster.count)} ×</span>
                <button type="button" class="link" onclick={() => app.select(cluster.item.index)}><ItemLabel row={cluster.item} /></button>
                <span class="mono muted path">{cluster.steps.join('  ')}</span>
              </li>
            {/each}
          </ol>
        {/if}
      </div>
    {/if}
    {#if series && (series.classes.length > 0 || series.containers.length > 0)}
      <div class="block">
        <h3>Growing at every step ({series.names.length} dumps)</h3>
        <p class="hint">Samples: {series.names.join(' → ')}. File modification times determine order and may not reflect capture order; bars are equally spaced samples, not a time scale. Only strictly increasing counts are shown; containers have at least 100 children and only the 2,000 largest are considered.</p>
        <ul class="list">
          {#each series.classes.slice(0, 15) as row (row.key)}
            <li>
              <button type="button" class="link" onclick={() => app.searchFor(row.key, row.isClass)}>{row.key}</button>
              <span class="num muted">{row.counts.map((count) => formatCount(count)).join(' → ')}</span>
            </li>
          {/each}
          {#each series.containers.slice(0, 10) as container (container.signature)}
            <li>
              <span class="mono path">{container.signature}</span>
              <span class="sample-bars" aria-hidden="true">{#each container.counts as count}<span style:height="{Math.max(3, count / container.counts.reduce((max, value) => Math.max(max, value), 1) * 32)}px"></span>{/each}</span>
              <span class="num muted">{container.counts.map((count) => formatCount(count)).join(' → ')} children</span>
            </li>
          {/each}
        </ul>
      </div>
    {/if}
    {#if series?.userClasses && series.userClasses.at(-1)! > series.userClasses[0]!}
      <p class="notice">
        User classes keep being compiled: {series.userClasses.map((count) => formatCount(count)).join(' → ')}. Classes
        declared at runtime (eval(), proxy generators) are never freed.
      </p>
    {/if}

    <div class="block">
      <h3>Largest changes</h3>
      <label>Measure <select bind:value={metric}><option value="sizeDelta">Self size</option><option value="countDelta">Count</option></select></label>
      <p class="hint">Changes from {app.baseline.name} to {app.overview?.fileName}. Self size excludes retained children. Showing up to 10 changes.</p>
      <ol class="change-chart">
        {#each [...shown].sort((a, b) => Math.abs(b[metric]) - Math.abs(a[metric])).slice(0, 10) as row (row.key + String(row.isClass))}
          {@const maximum = shown.reduce((max, item) => Math.max(max, Math.abs(item[metric])), 1)}
          <li>
            {#if row.isGone}<span>{row.key} (gone)</span>{:else}<button type="button" class="link" onclick={() => app.searchFor(row.key, row.isClass)}>{row.key}</button>{/if}
            <span class="change-track" aria-hidden="true"><span class:negative={row[metric] < 0} style:width="{Math.abs(row[metric]) / maximum * 50}%"></span></span>
            <span class="num">{formatDelta(row[metric], metric === 'sizeDelta' ? formatBytes : formatCount)}</span>
          </li>
        {/each}
      </ol>
    </div>
    <label class="toggle">
      <input type="checkbox" bind:checked={changedOnly} />
      Only types that changed
    </label>
    <VirtualTable
      label="Growth since the baseline"
      {columns}
      rows={shown}
      onActivate={(row) => !row.isGone && app.searchFor(row.key, row.isClass)}
      height="calc(100vh - 230px)"
    >
      {#snippet empty()}Nothing changed between the two dumps.{/snippet}
      {#snippet cell(row, column)}
        {#if column.key === 'key'}
          <span class:cls={row.isClass}>{row.key}</span>
          {#if row.isNew}<span class="badge new">new</span>{/if}
          {#if row.isGone}<span class="badge">gone</span>{/if}
        {:else if column.key === 'countDelta' || column.key === 'sizeDelta'}
          {@const delta = column.key === 'countDelta' ? row.countDelta : row.sizeDelta}
          <span class:grew={delta > 0} class:shrank={delta < 0}>{column.value(row)}</span>
        {:else}
          {column.value(row)}
        {/if}
      {/snippet}
    </VirtualTable>
  {/if}
</section>

<style>
  .sample-bars { display: inline-flex; align-items: end; gap: 3px; height: 32px; flex-shrink: 0; }
  .sample-bars span { display: block; width: 9px; background: var(--bar-strong); }
  .change-chart { display: grid; gap: var(--space-2); padding: 0; list-style: none; }
  .change-chart li { display: grid; grid-template-columns: minmax(120px, 1fr) minmax(100px, 2fr) auto; gap: var(--space-3); align-items: center; }
  .change-track { height: 12px; background: linear-gradient(to right, var(--surface-sunken) 49.8%, var(--border-strong) 50%, var(--surface-sunken) 50.2%); position: relative; }
  .change-track span { display: block; position: absolute; left: 50%; height: 100%; background: var(--bar-strong); }
  .change-track span.negative { left: auto; right: 50%; }
  @media (max-width: 700px) { .change-chart li { grid-template-columns: 1fr auto; } .change-track { grid-column: 1 / -1; } }
  .view {
    display: flex;
    flex-direction: column;
    gap: var(--space-4);
    padding: var(--space-5);
  }

  .head {
    display: flex;
    justify-content: space-between;
    align-items: flex-start;
    gap: var(--space-4);
    flex-wrap: wrap;
  }

  h2 {
    font-size: var(--text-xl);
    font-weight: 600;
  }

  .sub {
    margin-top: 2px;
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  .baseline {
    display: flex;
    align-items: center;
    gap: var(--space-2);
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  .empty {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    gap: var(--space-3);
    max-width: 60ch;
    color: var(--text-muted);
  }

  .toggle {
    display: inline-flex;
    align-items: center;
    gap: var(--space-2);
    font-size: var(--text-sm);
    color: var(--text-muted);
  }

  .cls {
    font-weight: 500;
  }

  .generations {
    border-collapse: collapse;
    font-size: var(--text-sm);
  }

  .generations th,
  .generations td {
    padding: 2px var(--space-3);
    border-bottom: 1px solid var(--border);
    text-align: left;
  }

  .generations .num {
    text-align: right;
  }

  .block {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
  }

  h3 {
    font-size: var(--text-md);
    font-weight: 600;
  }

  h4 {
    font-size: var(--text-sm);
    font-weight: 600;
  }

  .list {
    display: flex;
    flex-direction: column;
    gap: var(--space-1);
    list-style: none;
    padding: 0;
    font-size: var(--text-sm);
  }

  .list li {
    display: flex;
    align-items: center;
    gap: var(--space-2);
    min-width: 0;
  }

  .path {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: var(--text-xs);
  }

  .link {
    background: none;
    border: 0;
    padding: 0;
    color: var(--accent);
    cursor: pointer;
  }

  .grew {
    color: var(--danger);
    font-weight: 500;
  }

  .shrank {
    color: var(--success);
  }
</style>
