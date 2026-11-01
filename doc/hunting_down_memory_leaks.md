Hunting down memory leaks
=========================

This guide walks through finding a memory leak with meminfo and its browser UI: what to dump, when,
and how to read the result until the line of code that leaks is found.

What a leak looks like in PHP
-----------------------------

A PHP request rarely leaks in a way that matters: whatever it allocates is freed when it ends. Leaks
hurt in processes that live long — queue consumers, daemons, schedulers, long-running CLI commands,
event-loop servers, and php-fpm workers with a high `pm.max_requests`. There, memory that should be
freed after each job stays reachable from something that lives as long as the process:

- a static property or a global used as a cache that is never evicted;
- a listener, callback or logger handler registered once per job on a long-lived dispatcher;
- an identity map or unit of work (Doctrine's `EntityManager`, for example) that is never cleared;
- an array that collects "processed" ids, errors or log lines for the life of the process.

The symptoms are memory usage that grows with every job until `memory_limit` is hit, and more and
more time spent in the garbage collector, which keeps scanning objects it cannot free.

meminfo does not track allocations. It writes everything still reachable at the moment of the dump —
every array, object and value, the variable or static it hangs from, and what refers to what. A leak
is then something that is reachable but should not be, and the question becomes *what keeps it*.

Taking the dumps
----------------

Take **two dumps of the same process**: a baseline once it has warmed up, and a later one after many
more jobs. Whatever grew between the two is what leaks; whatever is merely big but stable is not.

From the code, at a point you choose (between two jobs, not in the middle of one):

```php
meminfo_dump(fopen('/tmp/worker-' . getmypid() . '-' . $jobs . '.json', 'w'));
```

From outside, without changing or restarting the process, with `dump-meminfo` (the target needs the
extension loaded, PHP 7.1+, non-ZTS, on POSIX):

```bash
$ dump-meminfo 12345 --dir=/tmp       # baseline
/tmp/meminfo-1834201933.json
$ dump-meminfo 12345 --dir=/tmp       # some minutes or jobs later
/tmp/meminfo-2911470521.json
```

A php-fpm worker can only be dumped while it is serving a request; take its pid from the FPM status
page, never the master's. Everything about the signal, the settings, FPM, containers and what to do
when the command refuses is in [Dumping a running process](process_dump.md).

A dump contains everything the process had in memory — keys, strings, tokens. Treat it as sensitive.

Opening a dump
--------------

Open <https://vantafinance.github.io/php-meminfo/> and drop the dump on the page (or use
**Open dump…**). The page is static: the file is read and analyzed by your browser, in a Web Worker,
and nothing is uploaded. To explore the interface without your own dump, choose **Try demo**. It opens synthetic sample snapshots; **Demo data** stays visible until you close or replace them.

To run the UI yourself (Node 24+):

```bash
$ make ui-install
$ make ui-dev        # open the printed URL
```

The left column lists the views: **Leak suspects**, **Growth**, **Summary**, **Top children**,
**Search** and **Graph**. Selecting any item in a table opens the inspector on the right.

A worked example
----------------

[`leak_example.php`](leak_example.php) is a small worker with a classic leak. It handles orders in a
loop; for each one it registers an `AuditListener` on a long-lived `Dispatcher`, and never removes it.
Each listener holds its `Order`, so no order is ever freed:

```php
function handle_order(Dispatcher $dispatcher, $id)
{
    $order = new Order($id);

    // The leak: a listener per order, registered on a long-lived dispatcher
    // and never removed. It holds the order, so no order is ever freed.
    $dispatcher->listen('order.shipped', new AuditListener($order));
}
```

It dumps after 100 orders and again after 1000:

```bash
$ php doc/leak_example.php /tmp
/tmp/leak-before.json
/tmp/leak-after.json
```

Open `/tmp/leak-after.json`.

### 1. Leak suspects: what keeps the most alive

**Leak suspects** ranks items by their *retained size*: how much memory would be freed if that item
went away, everything it alone keeps alive included. It is computed from the dominator tree of the
dump — item A dominates item B when every path from a root to B goes through A.

- **Largest retainers** visualizes the top ten individual retainers. Each bar is relative to the accounted dump size; bars may overlap and must not be added. Select an item or **Show graph** to investigate its references. The table below contains at most 500 suspects.
- **Biggest retainers** lists the items that dominate the most: here `Dispatcher dispatcher` in
  `<GLOBAL>`, holding over 99% of the dump, with about 19,000 items in **Items kept**. Long chains
  are folded into their head, so a linked list shows up once, not once per node.
- **Classes retaining the most** sums the same per class. `AuditListener` and `Order` both show
  1,000 **Instances**, all of them **Top-most** (not kept by another instance of the same class).
  Selecting a class row lists its instances in Search.
- **Roots by frame** shows which frames (`<GLOBAL>`, a function, a class's statics) hold the most.

Data reachable from several roots is dominated by none of them; it is flagged **shared** and listed
on its own rather than charged to any one root.

A big retainer is a lead, not a verdict: a large configuration array or a container is supposed to
be big. What matters is whether it grows.

### 2. Growth: compare with the baseline

In **Growth**, click **Choose baseline dump…** and pick `/tmp/leak-before.json`. Only the
baseline's totals per type and class are kept, so this does not need memory for a second dump.

The table lists, for each type or class, the count **Before**, **Now**, **Δ count** and
**Δ self size**; **Only types that changed** hides the rest. Here `AuditListener` and `Order` grew by
900 each — exactly the 900 orders handled between the two dumps — and arrays, strings and ints grew
by a multiple of that (the order lines). A count that grows in step with the work done is a useful lead, but it can also reflect legitimate retained data. Classes marked **new** appeared since the baseline, **gone** ones disappeared.
Selecting a row searches for its instances. **Largest changes** plots the largest positive and negative changes in count or self size, not retained size. For three or more snapshots, the sample bars show only counts that grew at every step. Files are ordered by modification time, which may differ from capture order; capture at comparable quiet points in the same process. Object handles can be reused, so three-snapshot candidates remain hypotheses.

### 3. Summary and Top children

**Summary** counts every item by type, objects by class, with **Count** and **Self size** and their
shares. On its own it answers "what is in memory"; it is most useful to check that a class you
expected to be gone (a request, an entity, a response) is still there in numbers.

**Top children** lists the arrays and objects with the most **Children**. A growing collection is the
usual shape of a leak, and here the top row is an array with 1,000 children retaining almost the
whole dump: the dispatcher's `order.shipped` listener list. Clicking its **Address** opens it in the
graph; selecting the row opens the inspector.

### 4. Search

**Search** finds items by field: `class`, `type`, `symbol`, `frame`, `is root`, `size`,
`object handle`, `address`. Every filter must match (**Add filter** for more); **is** compares
exactly and **matches** takes a regular expression. Searching `class` **is** `Order` returns 1,000
items. **Go to address** jumps straight to an item when you have its address from somewhere else.

Pick any `Order` from the results.

### 5. Inspector: why is it still here

The inspector shows the item's **Self size**, **Retained**, **Items kept alive**, **Distance from a
root**, **Children** and **Referenced by**, then:

- **Kept alive by** — the dominator chain from a root down to the item: each entry alone keeps the
  next one in memory. For our order it reads `Dispatcher dispatcher` → `array` → `array` →
  `AuditListener` → `Order`.
- **Reference paths from roots** — the actual paths, with the key or property at each step:

  ```
  Dispatcher dispatcher → [listeners] array → [order.shipped] array → [0] AuditListener → [order]
  ```

  This is the answer: the order is reachable because the dispatcher's `listeners['order.shipped']`
  still holds an `AuditListener`, whose `order` property holds it.
- **Children** and **Referenced by** — what the item holds and who holds it. Selecting a row moves
  the inspector there; **← Back** returns.

### 6. Graph: what to cut

Click **Show graph** in the inspector (or an address anywhere). In **Paths from roots** mode the graph
merges every shortest path from a root to the item, roots on the left and the item on the right.
Items that *every* path goes through are highlighted ("every path goes through it" in the legend):
free any one of them and the item goes. Items shared by several roots have their own colour.

With one root, every step is such a bottleneck; in real dumps there are usually several roots and
only a few highlighted items — those are where the fix goes. **Neighbourhood** draws the items around
one instead (**Depth** 1–3, **Follow** children, parents or both); double-click a node to bring in its
neighbours, click it to inspect it, **Fit** to re-centre, **Show as list** to go back to the
inspector. A graph is capped at 600 items, nearest roots first, and says so when it is cut short.

### 7. Fix and confirm

The listener list is the place to cut: the listener should be registered once, or removed when the
order is done. For example:

```php
$listener = new AuditListener($order);
$dispatcher->listen('order.shipped', $listener);
// ... handle the order ...
$dispatcher->forget('order.shipped', $listener);
```

Then take the two dumps again and load them as before. **Growth** should no longer show
`AuditListener` or `Order` growing between the baseline and the later dump, and **Leak suspects**
should no longer show the dispatcher retaining most of the dump. If something else now grows, that is
the next leak — repeat from step 1.

Faster ways in
--------------

The steps above work on any dump. With a newer extension and UI, these shortcuts often get you to the
holder in one click:

- **Findings first.** The Findings view lists what usually means a leak, with what to check next.
  For example: "array `Dispatcher::listeners` keeps 4 000 items alive, none of them big" (an
  accumulation point), or "2 000 closures from `OrderService.php:88` are alive" (listeners never
  removed). Duplicate strings show only in dumps taken with `string_hashes`, and unreachable cycles
  only with `unreachable`.
- **Three dumps instead of two.** `dump-meminfo <pid> --count=3 --interval=2m`, then open the three
  files together. Growth lists the objects created between the first two dumps and still alive in
  the third, and groups them by the path that holds them. Temporaries of the second dump are gone by
  the third, so what is left is what the work between them left behind.
- **How held.** On a class row of Leak suspects, or in the Inspector, "How held" groups the instances
  by the shape of their path from a root. A thousand leaked `Order`s fall into one or two groups, one
  per place in the code that keeps them.
- **Ignore what you know.** If a cache or the logger hides the real holder, "Ignore…" excludes it and
  everything is recomputed. The filter is kept in the URL.
- **Dump between jobs.** With `meminfo.dump_defer=1` and `meminfo_dump_if_requested()` in the worker
  loop, dumps contain no half-finished job, so Growth is cleaner
  ([recipes](process_dump.md#dumping-at-a-quiet-point-of-a-worker)).
- **Generations.** Call `meminfo_mark()` after each job (or set `meminfo.auto_mark=1`). The
  "Objects by generation" table then shows classes whose old generations stay populated.
- **Where it was created.** With `meminfo.track_allocations=1`, Leak suspects groups retained memory
  by the line of the `new` and by Composer package.
- **Histograms for production.** `meminfo_histogram()` written to a file every few minutes costs
  almost nothing. Open the files together ("Open histograms…") to see the classes whose count only
  goes up.
- **In CI.** `node ui/dist-report/cli.js after.json --baseline=before.json --max-growth=1000` fails
  the build when a class grows too much. See the README.
- **Try it all on one dump.** [`leaky_worker.php`](leaky_worker.php) leaks in every way the findings
  look for, and its header shows how to take a dump of any size from it.

Pitfalls
--------

- **Retained sizes are relative.** Item sizes are the allocations the extension sees: an object's
  struct, an array's slots and keys, a string's bytes. That is not allocator truth, so compare them
  with each other, not with `memory_usage`. Dumps from older extensions use rougher estimates, and
  the UI says so. An object first reached from a function's local may also appear without its later
  holders (a global or a static that also holds it emits no item of its own), so its retainer can be
  misplaced.
- **Memory outside PHP values is not in the dump.** Compiled code, the engine's own structures and
  buffers held by extensions (unfetched PDO/mysqli result sets, images in GD/Imagick) count towards
  `memory_get_usage()` but are not items. A large gap between **in use** in the top bar and what the
  dump accounts for points there.
- **Unreachable cycles are not in a default dump.** Objects that only refer to each other are garbage
  waiting for the cycle collector, and the walk from the roots never reaches them. Dump with
  `['unreachable' => true]` to list them under `<UNREACHABLE>`. If memory drops after
  `gc_collect_cycles()`, that was the cause, not a leak.
- **Idle processes do not serve signal dumps.** `dump-meminfo` is answered at the next opcode. An
  idle php-fpm worker has no handler installed, and a worker or event loop blocked in `sleep()`,
  `stream_select()` or another long internal call is woken by the signal but only dumps once it gets
  back to PHP code. Ask while it is busy, or dump from the code.
- **Swoole: turn on `swoole.enable_fiber_mock=1`** (PHP 8.1+) in the workers you dump. Without it the
  locals of suspended coroutines — often where a per-request leak sits — are not in the dump, while
  `memory_usage` still counts them. See [Dumping a running process](process_dump.md#limitations).
- **Very large dumps need browser memory.** The UI streams the file into compact typed arrays, but a
  dump of tens of millions of items can still exceed what a tab may hold (Chrome manages about
  2.5 GB of dump). The UI warns before loading a file that large and says so if a tab ran out of
  memory. Dump earlier, or dump a smaller process (fewer jobs, a smaller batch) — a leak shows in
  Growth long before it fills the memory limit.
