<script lang="ts">
  import before from '../demo/before.json?raw'
  import after from '../demo/after.json?raw'
  import { formatBytes } from '../format'
  import { app } from '../state/app.svelte'

  /** The README on GitHub holds the full reference; links only, nothing is fetched. */
  const REPO = 'https://github.com/VantaFinance/php-meminfo'

  let input: HTMLInputElement | undefined = $state()
  let dragging = $state(false)

  function pick(event: Event): void {
    const target = event.currentTarget as HTMLInputElement
    const files = [...(target.files ?? [])]
    // Drop the input's reference at once: the File must not outlive the hand-off.
    target.value = ''
    if (files.length > 0) app.openSeries(files)
  }

  function openDemo(): void {
    app.openSeries([
      new File([before], 'demo-before.json', { type: 'application/json', lastModified: 1 }),
      new File([after], 'demo-after.json', { type: 'application/json', lastModified: 2 }),
    ])
    app.isDemo = true
  }

  function drop(event: DragEvent): void {
    event.preventDefault()
    dragging = false
    const files = [...(event.dataTransfer?.files ?? [])]
    if (files.length > 0) app.openSeries(files)
  }
</script>

<div class="page">
  <section class="panel">
    <div class="intro">
      <h1>PHP Meminfo</h1>
      <p class="lead">Find what keeps memory alive in a PHP process, from a meminfo dump.</p>
    </div>

    {#if app.error}
      <div class="notice danger" role="alert">
        <strong>{app.error.fileName} could not be opened.</strong>
        {app.error.message}
      </div>
    {/if}

    {#if app.pendingLarge}
      <div class="notice warning" role="alert">
        <p>
          <strong>{app.pendingLarge.name}</strong> is {formatBytes(app.pendingLarge.size)} — more than this browser is
          expected to hold (about 2.5 GB of dump in Chrome). Loading it may run out of memory.
        </p>
        <div class="actions">
          <button type="button" class="button" onclick={() => app.pendingLarge && app.open(app.pendingLarge, true)}>Load anyway</button>
          <button type="button" class="button quiet" onclick={() => (app.pendingLarge = null)}>Cancel</button>
        </div>
      </div>
    {/if}

    <div
      class="drop"
      class:dragging
      role="region"
      aria-label="Drop a dump file here"
      ondragover={(event) => {
        event.preventDefault()
        dragging = true
      }}
      ondragleave={() => (dragging = false)}
      ondrop={drop}
    >
      <p class="drop-title">Drop a dump file here</p>
      <p class="drop-hint">or</p>
      <button type="button" class="button primary" onclick={() => input?.click()}>Open dump…</button>
      <input bind:this={input} class="visually-hidden" type="file" multiple accept=".json,application/json" onchange={pick} />
      <button type="button" class="button quiet" onclick={openDemo}>Try demo</button>
      <p class="privacy">The dump is read and analyzed in this browser. Nothing is uploaded.</p>
    </div>
  </section>

  <div class="guide">
    <section class="about" aria-labelledby="about-title">
      <h2 id="about-title">What it is</h2>
      <p>
        <a href={REPO} rel="noreferrer">meminfo</a> is a PHP extension that writes everything a PHP process holds in
        memory to a JSON file: each array, object and value, the variable or static it belongs to, and what refers to
        what. This page reads such a dump and shows what keeps memory alive — the biggest retainers, what grew since an
        earlier dump, and every path from a root to an item. The file is read by this browser and never leaves it.
      </p>
    </section>

    <section class="install" aria-labelledby="install-title">
      <h2 id="install-title">Install the extension</h2>
      <p>Build it from the <code>extension</code> directory of the repository:</p>
      <pre><code>phpize
./configure --enable-meminfo
make
make install</code></pre>
      <p>Then enable it in <code>php.ini</code>:</p>
      <pre><code>extension=meminfo.so</code></pre>
      <p>
        PHP 7.0 – 8.5. Dumping another, running process needs the target to be PHP 7.1 or later, non-ZTS, on a POSIX
        system.
      </p>
      <p class="more">
        <a href="{REPO}#compilation" rel="noreferrer">Compilation</a> ·
        <a href="{REPO}#enabling-the-extension" rel="noreferrer">Enabling the extension</a>
      </p>
    </section>

    <section class="settings" aria-labelledby="settings-title">
      <h2 id="settings-title">Settings</h2>
      <p>Both are read at startup (<code>PHP_INI_SYSTEM</code>), so they belong in <code>php.ini</code>:</p>
      <div class="table-wrap">
        <table>
          <thead>
            <tr><th scope="col">Setting</th><th scope="col">Default</th><th scope="col">Meaning</th></tr>
          </thead>
          <tbody>
            <tr>
              <th scope="row"><code>meminfo.dump_signal</code></th>
              <td><code>SIGURG</code></td>
              <td>Signal the process dumps on, by name (with or without <code>SIG</code>) or number. <code>0</code> or empty turns it off.</td>
            </tr>
            <tr>
              <th scope="row"><code>meminfo.dump_dir</code></th>
              <td>system temp dir</td>
              <td>
                Where dumps are written unless the trigger passes <code>--dir</code>. On macOS the temp dir differs per
                user: set it, or pass <code>--dir</code>.
              </td>
            </tr>
          </tbody>
        </table>
      </div>
      <pre><code>extension=meminfo.so
meminfo.dump_signal=SIGURG
meminfo.dump_dir=/var/tmp/meminfo</code></pre>
      <p>
        <code>SIGURG</code> is ignored by default, so signalling a process that is not listening does nothing;
        <code>SIGUSR1</code>/<code>SIGUSR2</code> would kill an idle php-fpm worker. <code>php -i | grep meminfo</code>
        shows what a build resolved.
      </p>
      <p class="more"><a href="{REPO}#settings" rel="noreferrer">Settings</a></p>
    </section>

    <section class="dump" aria-labelledby="dump-title">
      <h2 id="dump-title">Getting a dump</h2>
      <p>From the application itself:</p>
      <pre><code>meminfo_dump(fopen('/tmp/dump.json', 'w'));</code></pre>
      <p>From a running process, without changing it — the command prints the path of the dump:</p>
      <pre><code>dump-meminfo &lt;pid&gt; --dir=/tmp</code></pre>
      <p>Or from PHP:</p>
      <pre><code>$path = meminfo_dump_process($pid, ['dir' =&gt; '/tmp']);</code></pre>
      <p>
        To see what grows, take one dump, let the process run, take another, and compare them in the
        <em>Growth</em> view.
      </p>
      <p class="more">
        <a href="{REPO}#dumping-memory-content" rel="noreferrer">Dumping memory content</a> ·
        <a href="{REPO}#dumping-a-running-process" rel="noreferrer">Dumping a running process</a>
      </p>
    </section>
  </div>
</div>

<style>
  /* One column, left edges aligned: the drop zone first, the reference below it. */
  .page {
    display: flex;
    flex-direction: column;
    gap: var(--space-6);
    width: min(960px, 100%);
    margin: 0 auto;
    padding: 10vh var(--space-4) var(--space-6);
    box-sizing: border-box;
  }

  .panel {
    display: flex;
    flex-direction: column;
    gap: var(--space-5);
    width: min(640px, 100%);
  }

  h1 {
    font-size: var(--text-2xl);
    font-weight: 650;
    letter-spacing: -0.01em;
  }

  .lead {
    margin-top: var(--space-1);
    color: var(--text-muted);
    font-size: var(--text-lg);
  }

  .drop {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--space-2);
    padding: var(--space-6) var(--space-5);
    border: 1.5px dashed var(--border-strong);
    border-radius: var(--radius-md);
    background: var(--surface-sunken);
    transition: border-color var(--motion), background var(--motion);
  }

  .drop.dragging {
    border-color: var(--accent);
    background: var(--accent-soft);
  }

  .drop-title {
    font-size: var(--text-lg);
    font-weight: 600;
  }

  .drop-hint,
  .privacy {
    color: var(--text-muted);
    font-size: var(--text-sm);
  }

  .privacy {
    margin-top: var(--space-2);
  }

  /* The reference below the drop zone: one column on phones, two from 900px. */
  .guide {
    display: grid;
    grid-template-columns: minmax(0, 1fr);
    gap: var(--space-6) var(--space-6);
    padding-top: var(--space-5);
    border-top: 1px solid var(--border);
  }

  @media (min-width: 900px) {
    .guide {
      grid-template-columns: repeat(2, minmax(0, 1fr));
      grid-template-areas:
        'about about'
        'install settings'
        'dump settings';
      align-items: start;
    }

    .about {
      grid-area: about;
      max-width: 72ch;
    }

    .install {
      grid-area: install;
    }

    .settings {
      grid-area: settings;
    }

    .dump {
      grid-area: dump;
    }
  }

  .guide section {
    display: flex;
    flex-direction: column;
    gap: var(--space-2);
    min-width: 0;
    color: var(--text-muted);
    font-size: var(--text-sm);
    line-height: 1.55;
  }

  .about p {
    font-size: var(--text-md);
  }

  code {
    font-family: var(--font-mono);
    font-size: 0.95em;
    color: var(--text);
  }

  table {
    width: 100%;
    border-collapse: collapse;
  }

  th,
  td {
    padding: var(--space-2) var(--space-2) var(--space-2) 0;
    border-bottom: 1px solid var(--border);
    text-align: left;
    vertical-align: top;
  }

  thead th {
    color: var(--text-faint);
    font-size: var(--text-xs);
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }

  tbody th {
    font-weight: 400;
    white-space: nowrap;
  }

  .table-wrap {
    overflow-x: auto;
  }

  a {
    color: var(--accent);
  }

  .more {
    margin-top: var(--space-1);
  }

  h2 {
    color: var(--text);
    font-size: var(--text-md);
    font-weight: 600;
  }

  pre {
    margin: 0;
    padding: var(--space-2) var(--space-3);
    border-radius: var(--radius-sm);
    background: var(--surface-sunken);
    color: var(--text);
    overflow-x: auto;
  }

  .notice p {
    margin-bottom: var(--space-2);
  }

  .actions {
    display: flex;
    gap: var(--space-2);
  }
</style>
