<script lang="ts">
  import { formatCount } from '../../format'
  import { createLogger } from '../../log'
  import { app } from '../../state/app.svelte'
  import { WorkerFailure } from '../../worker/client'
  import type { GraphDirection, GraphMode, GraphResult } from '../../worker/protocol'
  import GraphCanvas from '../graph/GraphCanvas.svelte'

  const log = createLogger('graph-view')

  /** What one request may bring back, and what the picture may grow to with expansions. */
  const FIRST_NODES = 300
  const EXPAND_NODES = 60
  const MAX_NODES = 600

  let mode = $state<GraphMode>('paths')
  let depth = $state(1)
  let direction = $state<GraphDirection>('both')
  let result = $state.raw<GraphResult | null>(null)
  let failed = $state<string | null>(null)
  let capped = $state(false)
  let canvas: GraphCanvas | undefined = $state()

  $effect(() => {
    void app.generation
    const client = app.client
    const target = app.graphTarget
    const request = { mode, depth, direction }
    result = null
    failed = null
    capped = false
    if (client === null || target === null) return
    let cancelled = false
    log.info('graph requested', { target, ...request })
    client
      .request({ kind: 'graph', index: target, ...request, maxNodes: FIRST_NODES })
      .then((value) => {
        if (!cancelled) result = value
      })
      .catch((error: unknown) => {
        if (!cancelled) failed = error instanceof WorkerFailure ? error.message : String(error)
      })
    return () => {
      cancelled = true
    }
  })

  /** Merge the neighbours of a node into the picture. */
  async function expand(index: number): Promise<void> {
    const client = app.client
    const current = result
    if (client === null || current === null) return
    if (current.nodes.length >= MAX_NODES) {
      capped = true
      return
    }
    log.debug('expanding', { index })
    const more = await client.request({ kind: 'graph', index, mode: 'neighbourhood', depth: 1, direction: 'both', maxNodes: EXPAND_NODES })
    if (result !== current) return // the graph changed meanwhile

    const known = new Set(current.nodes.map((node) => node.index))
    const nodes = [...current.nodes]
    for (const node of more.nodes) {
      if (known.has(node.index)) continue
      if (nodes.length >= MAX_NODES) {
        capped = true
        break
      }
      known.add(node.index)
      // The expansion's centre is not the graph's target.
      nodes.push({ ...node, isTarget: false })
    }
    const edgeKey = (edge: { source: number; target: number; name: string }): string => `${edge.source}>${edge.target}>${edge.name}`
    const edges = [...current.edges]
    const seen = new Set(edges.map(edgeKey))
    for (const edge of more.edges) {
      if (!known.has(edge.source) || !known.has(edge.target) || seen.has(edgeKey(edge))) continue
      seen.add(edgeKey(edge))
      edges.push(edge)
    }
    result = { ...current, nodes, edges, truncated: current.truncated || more.truncated }
  }

  const target = $derived(result?.nodes.find((node) => node.isTarget))
</script>

<section class="view">
  <header class="head">
    <div>
      <h2>Graph</h2>
      <p class="sub">
        {#if mode === 'paths'}
          Shortest reference paths from roots to the selected item, merged. Arrows point from holder to held item. Other, longer paths may exist.
        {:else}
          The items around this one. Double-click any node to bring in its neighbours.
        {/if}
      </p>
    </div>
  </header>

  {#if app.graphTarget === null}
    <div class="empty">
      <p>Pick an item to draw: click an address in Search or Top children, or use “Show graph” in an item's details.</p>
    </div>
  {:else}
    {#if app.retainerFilters.length > 0}
      <p class="notice">Ignored holders are active: root paths and retained sizes are recalculated without those references. Neighbourhood mode shows original references.</p>
    {/if}
    <div class="toolbar" role="toolbar" aria-label="Graph options">
      <div class="segmented" role="group" aria-label="Mode">
        <button type="button" class:on={mode === 'paths'} aria-pressed={mode === 'paths'} onclick={() => (mode = 'paths')}>Paths from roots</button>
        <button type="button" class:on={mode === 'neighbourhood'} aria-pressed={mode === 'neighbourhood'} onclick={() => (mode = 'neighbourhood')}>Neighbourhood</button>
      </div>
      {#if mode === 'neighbourhood'}
        <label>
          Depth
          <select bind:value={depth}>
            <option value={1}>1</option>
            <option value={2}>2</option>
            <option value={3}>3</option>
          </select>
        </label>
        <label>
          Follow
          <select bind:value={direction}>
            <option value="both">children and parents</option>
            <option value="children">children</option>
            <option value="parents">parents</option>
          </select>
        </label>
      {/if}
      <span class="spacer"></span>
      <button type="button" class="button quiet" onclick={() => canvas?.fit()} disabled={result === null}>Fit</button>
      <button type="button" class="button quiet" onclick={() => app.graphTarget !== null && app.select(app.graphTarget)}>Show as list</button>
    </div>

    {#if failed}
      <div class="notice danger" role="alert">{failed}</div>
    {:else if result === null}
      <div class="skeleton placeholder"></div>
    {:else}
      <p class="summary" data-testid="graph-summary">
        {formatCount(result.nodes.length)} items · {formatCount(result.edges.length)} references
        {#if result.mode === 'paths'}· {formatCount(result.totalRoots)} {result.totalRoots === 1 ? 'root reaches' : 'roots reach'} {target?.className ?? target?.type ?? 'it'}{/if}
      </p>
      {#if result.truncated || capped}
        <p class="notice warning" role="status">
          Only part of the graph is shown: it is capped at {formatCount(MAX_NODES)} items so the picture stays readable.
          {#if result.mode === 'paths'}The nearest roots come first.{/if}
        </p>
      {/if}
      <div class="stage">
        <GraphCanvas
          bind:this={canvas}
          {result}
          selected={app.selected}
          onSelect={(index) => app.select(index)}
          onExpand={(index) => void expand(index)}
          onError={(message) => (failed = message)}
        />
      </div>
      <ul class="legend" aria-label="Legend">
        <li><span class="swatch target"></span>the item</li>
        <li><span class="swatch root"></span>root</li>
        <li><span class="swatch bottleneck"></span>every root path goes through it</li>
        <li><span class="swatch shared"></span>shared by several roots</li>
      </ul>
    {/if}
  {/if}
</section>

<style>
  .view {
    display: flex;
    flex-direction: column;
    gap: var(--space-3);
    height: 100%;
    padding: var(--space-5);
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

  .empty {
    max-width: 60ch;
    color: var(--text-muted);
  }

  .toolbar {
    display: flex;
    align-items: center;
    gap: var(--space-3);
    flex-wrap: wrap;
    font-size: var(--text-sm);
  }

  .toolbar label {
    display: inline-flex;
    align-items: center;
    gap: var(--space-2);
    color: var(--text-muted);
  }

  .spacer {
    flex: 1;
  }

  .segmented {
    display: inline-flex;
    border: 1px solid var(--border-strong);
    border-radius: var(--radius-md);
    overflow: hidden;
  }

  .segmented button {
    height: 28px;
    padding: 0 var(--space-3);
    border: 0;
    background: var(--surface-raised);
    color: var(--text-muted);
    font: inherit;
    cursor: pointer;
    transition: background var(--motion);
  }

  .segmented button + button {
    border-left: 1px solid var(--border-strong);
  }

  .segmented button:hover {
    background: var(--surface-hover);
  }

  .segmented button.on {
    background: var(--accent-soft);
    color: var(--accent);
    font-weight: 600;
  }

  .summary {
    color: var(--text-muted);
    font-size: var(--text-sm);
    font-variant-numeric: tabular-nums;
  }

  .stage {
    flex: 1;
    min-height: 360px;
  }

  .placeholder {
    flex: 1;
    min-height: 360px;
  }

  .legend {
    display: flex;
    flex-wrap: wrap;
    gap: var(--space-4);
    margin: 0;
    padding: 0;
    list-style: none;
    color: var(--text-muted);
    font-size: var(--text-xs);
  }

  .legend li {
    display: inline-flex;
    align-items: center;
    gap: var(--space-2);
  }

  .swatch {
    width: 14px;
    height: 10px;
    border-radius: 3px;
    border: 2px solid var(--border-strong);
    background: var(--surface-raised);
  }

  .swatch.target {
    border-color: var(--danger);
  }

  .swatch.root {
    border-color: var(--accent);
  }

  .swatch.bottleneck {
    border-color: var(--accent);
    background: var(--accent-soft);
  }

  .swatch.shared {
    border-color: var(--warning);
    border-style: dashed;
  }
</style>
