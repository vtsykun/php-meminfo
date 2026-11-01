<script lang="ts">
  import { formatBytes, formatCount, formatPercent } from '../../format'
  import { createLogger } from '../../log'
  import { app } from '../../state/app.svelte'
  import { sizesAreAllocations, unattributedMemory } from '../../engine/store'
  import type { ClassRetentionRow, SuspectRow, SuspectsResult } from '../../worker/protocol'
  import type { SitesReport } from '../../engine/analyses/sites'
  import ItemLabel from '../ItemLabel.svelte'
  import RetainersPanel from '../RetainersPanel.svelte'
  import VirtualTable, { type Column } from '../VirtualTable.svelte'

  const log = createLogger('suspects-view')

  let result = $state.raw<SuspectsResult | null>(null)
  let failed = $state<string | null>(null)

  $effect(() => {
    void app.generation
    const client = app.client
    if (client === null) return
    let cancelled = false
    client
      .request({ kind: 'suspects', limit: 500 })
      .then((value) => {
        if (!cancelled) result = value
      })
      .catch((error: unknown) => {
        if (cancelled) return
        failed = String(error)
        log.warn('suspects failed', { error: failed })
      })
    return () => {
      cancelled = true
    }
  })

  const total = $derived(result?.totalSize ?? 0)

  // Where objects were created, when the dump was taken with meminfo.track_allocations.
  let sites = $state.raw<SitesReport | null>(null)
  $effect(() => {
    void app.generation
    const client = app.client
    sites = null
    if (client === null) return
    let cancelled = false
    client
      .request({ kind: 'sites' })
      .then((value) => {
        if (!cancelled) sites = value
      })
      .catch((error: unknown) => log.warn('sites failed', { error: String(error) }))
    return () => {
      cancelled = true
    }
  })
  const unattributed = $derived(app.overview && result ? unattributedMemory(app.overview.header, result.totalSize) : null)

  $effect(() => {
    if (unattributed !== null && app.overview?.header.memory_usage) {
      log.info('unattributed memory', { share: unattributed / app.overview.header.memory_usage })
    }
  })

  const classColumns: Column<ClassRetentionRow>[] = [
    { key: 'key', label: 'Class or type', width: 'minmax(160px, 1fr)', value: (row) => row.key },
    { key: 'retained', label: 'Retained', width: '96px', align: 'right', value: (row) => formatBytes(row.retained), compare: (a, b) => a.retained - b.retained },
    { key: 'share', label: 'Share', width: '120px', value: (row) => formatPercent(row.retained, total) },
    { key: 'instances', label: 'Instances', width: '96px', align: 'right', value: (row) => formatCount(row.instances), compare: (a, b) => a.instances - b.instances },
    {
      key: 'topMost',
      label: 'Top-most',
      width: '88px',
      align: 'right',
      value: (row) => formatCount(row.topMost),
      title: () => 'Instances not kept alive by another instance of the same class',
      compare: (a, b) => a.topMost - b.topMost,
    },
    { key: 'how', label: '', width: '104px', value: () => 'How held' },
  ]

  const suspectColumns: Column<SuspectRow>[] = [
    { key: 'what', label: 'Retainer', width: 'minmax(200px, 1fr)', value: (row) => `${row.className ?? row.type} ${row.symbol ?? ''}` },
    { key: 'frame', label: 'Frame', width: 'minmax(160px, 0.7fr)', value: (row) => row.frame ?? (row.shared ? 'shared by several roots' : '') },
    { key: 'retained', label: 'Retained', width: '96px', align: 'right', value: (row) => formatBytes(row.retained), compare: (a, b) => a.retained - b.retained },
    { key: 'share', label: 'Share', width: '120px', value: (row) => formatPercent(row.retained, total) },
    { key: 'dominated', label: 'Items kept', width: '96px', align: 'right', value: (row) => formatCount(row.dominated), compare: (a, b) => a.dominated - b.dominated },
  ]
</script>

{#snippet share(part: number)}
  <span class="share">
    <span class="share-bar" style:width="{total > 0 ? Math.max(2, (part / total) * 100) : 0}%"></span>
    <span class="share-text">{formatPercent(part, total)}</span>
  </span>
{/snippet}

<section class="view">
  <header class="view-header">
    <div>
      <h2>Leak suspects</h2>
      <p class="sub">Start with a retainer, compare growth, then open its graph to see what keeps it reachable.</p>
    </div>
  </header>

  {#if failed}
    <div class="notice danger" role="alert">{failed}</div>
  {:else if result === null}
    <div class="loading">
      <div class="skeleton" style:height="18px" style:width="40%"></div>
      <div class="skeleton" style:height="160px"></div>
    </div>
  {:else}
    <p class="notice">
      The dump accounts for {formatBytes(result.totalSize)}{#if app.overview?.header.memory_usage}{' '}of the
        {formatBytes(app.overview.header.memory_usage)} PHP reported in use{/if}.
      {#if app.overview && sizesAreAllocations(app.overview.header)}
        Sizes are the allocations items stand for; values stored inside an array or an object are counted by their holder.
      {:else}
        This dump comes from an older extension: sizes are rough estimates, and a string or array shared by several
        holders is counted once per holder.
      {/if}
      A value also held by a global or a static after a local may be missing its other holders, so treat retained sizes
      as relative.
      {#if unattributed && app.overview?.header.memory_usage}
        {formatBytes(unattributed)} ({formatPercent(unattributed, app.overview.header.memory_usage)}) of PHP's memory is
        held by no item: extension internals (mysqlnd buffered results, DOM documents, image libraries), compiled code{#if app.overview.header.user_class_count}{' '}({formatCount(app.overview.header.user_class_count)} user classes){/if},
        the VM stack and allocator overhead.
      {/if}
      {#if result.sharedItems > 0}
        {formatCount(result.sharedItems)} items are shared by several roots and retained by none of them alone.
      {/if}
    </p>

    <div class="block">
      <h3>Largest retainers</h3>
      <p class="hint">Each bar shows retained memory as a share of the accounted dump. Retainers may overlap; their sizes cannot be added. Showing up to 10 of the first 500 suspects.</p>
      <ol class="retainer-chart" aria-label="Largest retainers by retained memory">
        {#each result.suspects.slice(0, 10) as row (row.index)}
          <li>
            <button type="button" class="chart-label" aria-pressed={app.selected === row.index} onclick={() => app.select(row.index)}><ItemLabel {row} /></button>
            <span class="chart-track" aria-hidden="true"><span style:width="{total > 0 ? Math.min(100, row.retained / total * 100) : 0}%"></span></span>
            <span class="num">{formatBytes(row.retained)} ({formatCount(row.retained)} B) · {formatPercent(row.retained, total)}</span>
            <button type="button" class="button quiet" aria-label="Open reference graph for {row.className ?? row.type} {row.symbol ?? ''}" onclick={() => app.openGraph(row.index)}>Graph</button>
          </li>
        {/each}
      </ol>
    </div>
    <div class="block">
      <h3>Biggest retainers</h3>
      <VirtualTable
        label="Biggest retainers"
        columns={suspectColumns}
        rows={result.suspects}
        rowKey={(row) => row.index}
        selectedKey={app.selected}
        onActivate={(row) => app.select(row.index)}
        height="320px"
      >
        {#snippet cell(row, column)}
          {#if column.key === 'what'}
            <ItemLabel {row} />
            {#if row.shared}<span class="badge shared">shared</span>{/if}
          {:else if column.key === 'share'}
            {@render share(row.retained)}
          {:else}
            {column.value(row)}
          {/if}
        {/snippet}
      </VirtualTable>
    </div>

    <div class="block">
      <h3>Classes retaining the most</h3>
      <VirtualTable
        label="Classes retaining the most"
        columns={classColumns}
        rows={result.classes}
        onActivate={(row) => app.searchFor(row.key, row.isClass)}
        height="320px"
      >
        {#snippet cell(row, column)}
          {#if column.key === 'share'}
            {@render share(row.retained)}
          {:else if column.key === 'how'}
            <button
              type="button"
              class="button quiet"
              onclick={(event) => {
                event.stopPropagation()
                app.showRetainers({ key: row.key, isClass: row.isClass })
              }}>How held</button
            >
          {:else}
            {column.value(row)}
          {/if}
        {/snippet}
      </VirtualTable>
    </div>

    {#if app.retainersFor}
      <RetainersPanel target={app.retainersFor} />
    {/if}

    {#if sites && sites.sites.length > 0}
      <div class="block">
        <h3>Where the memory was allocated</h3>
        <p class="hint">
          {formatCount(sites.tracked)} of {formatCount(sites.objects)} objects have the line of the <code>new</code> that
          created them (meminfo.track_allocations); objects made otherwise (unserialize, Reflection, extensions) have none.
        </p>
        <ul class="frames">
          {#each sites.packages.slice(0, 8) as row (row.key)}
            <li>
              <span class="mono">{row.key}</span>
              <span class="muted">{formatCount(row.objects)} objects</span>
              <span class="num">{formatBytes(row.retained)}</span>
            </li>
          {/each}
        </ul>
        <ul class="frames">
          {#each sites.sites.slice(0, 15) as row (row.key)}
            <li>
              <span class="mono" title={row.key}>{row.key.slice(row.key.lastIndexOf('/') + 1)}</span>
              <span class="muted">{formatCount(row.objects)} objects</span>
              <span class="num">{formatBytes(row.retained)}</span>
            </li>
          {/each}
        </ul>
      </div>
    {/if}

    {#if result.frames.length > 0}
      <div class="block">
        <h3>Roots by frame</h3>
        <ul class="frames">
          {#each result.frames.slice(0, 12) as frame (frame.frame)}
            <li>
              <span class="mono">{frame.frame}</span>
              <span class="muted">{formatCount(frame.roots)} roots</span>
              <span class="num">{formatBytes(frame.retained)}</span>
            </li>
          {/each}
        </ul>
      </div>
    {/if}
  {/if}
</section>

<style>
  .retainer-chart { display: grid; gap: var(--space-2); list-style: none; padding: 0; margin: 0; }
  .retainer-chart li { display: grid; grid-template-columns: minmax(140px, 1fr) minmax(80px, 2fr) auto auto; gap: var(--space-3); align-items: center; }
  .chart-label[aria-pressed="true"] { font-weight: 700; text-decoration: underline; }
  .chart-label:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  .chart-label { text-align: left; background: none; border: 0; color: var(--accent); cursor: pointer; overflow-wrap: anywhere; }
  .chart-track { height: 12px; background: var(--surface-sunken); border-radius: 6px; overflow: hidden; }
  .chart-track span { display: block; height: 100%; background: var(--bar-strong); }
  @media (max-width: 700px) { .retainer-chart li { grid-template-columns: 1fr auto; } .chart-track { grid-column: 1 / -1; } }
  .view {
    display: flex;
    flex-direction: column;
    gap: var(--space-4);
    padding: var(--space-5);
  }

  .view-header h2 {
    font-size: var(--text-xl);
    font-weight: 600;
  }

  .sub {
    margin-top: 2px;
    color: var(--text-muted);
    font-size: var(--text-sm);
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

  .loading {
    display: flex;
    flex-direction: column;
    gap: var(--space-3);
  }

  .share {
    display: flex;
    align-items: center;
    gap: var(--space-2);
    width: 100%;
  }

  .share-bar {
    height: 6px;
    max-width: 60%;
    border-radius: 3px;
    background: var(--bar-strong);
  }

  .share-text {
    color: var(--text-muted);
    font-size: var(--text-xs);
    font-variant-numeric: tabular-nums;
  }

  .frames {
    display: grid;
    gap: 2px;
    margin: 0;
    padding: 0;
    list-style: none;
    font-size: var(--text-sm);
  }

  .frames li {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 100px 96px;
    gap: var(--space-3);
    padding: 4px var(--space-3);
    border-radius: var(--radius-sm);
  }

  .frames li:nth-child(odd) {
    background: var(--surface-sunken);
  }

  .muted {
    color: var(--text-muted);
  }

  .num {
    text-align: right;
    font-variant-numeric: tabular-nums;
  }
</style>
