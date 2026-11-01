import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { DumpParser } from '../parser'
import type { DumpStore } from '../store'
import { compareWithBaseline, growthSeries, leakCandidates, reduceToBaseline } from './diff'

function load(name: string): DumpStore {
  const bytes = new Uint8Array(readFileSync(new URL(`../../../../tests/fixtures/parity/${name}.json`, import.meta.url)))
  const parser = new DumpParser(bytes.length)
  parser.push(bytes)
  return parser.finish()
}

describe('compareWithBaseline', () => {
  const baseline = reduceToBaseline(load('leak-before'), 'leak-before.json')
  const rows = compareWithBaseline(baseline, load('leak-after'))

  it('keeps only aggregates of the baseline, and 8 bytes per object', () => {
    expect(baseline.items).toBe(400)
    expect(JSON.stringify(baseline.rows).length).toBeLessThan(2_000)
    const objects = baseline.objects!
    expect(objects.handles.byteLength + objects.classes.byteLength).toBe(objects.handles.length * 8)
  })

  it('puts the leaking class among the most grown, by 150 instances', () => {
    const request = rows.find((row) => row.isClass && row.key === 'Request')

    expect(request).toMatchObject({ before: { count: 50 }, after: { count: 200 }, countDelta: 150, isNew: false })
    expect(rows.slice(0, 3).map((row) => row.key)).toContain('Request')
  })

  it('does not report the steady array as growing', () => {
    expect(rows.every((row) => row.countDelta >= 0)).toBe(true)
    expect(rows[0]!.countDelta).toBeGreaterThanOrEqual(rows.at(-1)!.countDelta)
  })

  it('marks new and gone types', () => {
    const synthetic = compareWithBaseline(
      { name: 'b', items: 1, totalSize: 16, rows: [{ key: 'Old', isClass: true, count: 1, selfSize: 72 }] },
      load('leak-before'),
    )

    expect(synthetic.find((row) => row.key === 'Old')).toMatchObject({ isGone: true, countDelta: -1 })
    expect(synthetic.find((row) => row.key === 'Request')).toMatchObject({ isNew: true, countDelta: 50 })
  })
})

/** A dump of objects given as [handle, class], all held by one root array $all. */
function objectsDump(objects: Array<[number, string]>): DumpStore {
  const address = (i: number): string => '0x' + (0x2000 + i * 8).toString(16)
  const children = objects.map((_, k) => `"${k}":"${address(k + 1)}"`).join(',')
  const items = [`"${address(0)}":{"type":"array","size":"72","symbol_name":"all","is_root":true,"frame":"<GLOBAL>","children":{${children}}}`]
  objects.forEach(([handle, className], k) => {
    items.push(`"${address(k + 1)}":{"type":"object","size":"72","is_root":false,"class":"${className}","object_handle":"${handle}","children":{}}`)
  })
  const parser = new DumpParser()
  parser.push(new TextEncoder().encode(`{"header":{"user_class_count":10},"items":{${items.join(',')}}}`))
  return parser.finish()
}

describe('leakCandidates', () => {
  // A: the service. B: the service, a temporary and a leak. C: the service, the
  // leak, and a new temporary. Handle 2 is reused by C for another class.
  const first = reduceToBaseline(objectsDump([[1, 'Service']]), 'a.json')
  const second = reduceToBaseline(objectsDump([[1, 'Service'], [2, 'Temporary'], [3, 'Leak']]), 'b.json')
  const current = objectsDump([[1, 'Service'], [3, 'Leak'], [2, 'OtherTemporary']])

  it('keeps objects created between the first two dumps and still alive', () => {
    const candidates = leakCandidates(first, second, current)
    expect(candidates.map((index) => current.className(index))).toEqual(['Leak'])
  })

  it('does not match a reused handle of another class', () => {
    const candidates = leakCandidates(first, second, current)
    expect(candidates.map((index) => current.handle[index])).not.toContain(2)
  })

  it('cannot distinguish a reused handle of the same class from a survivor', () => {
    const original = reduceToBaseline(objectsDump([]), 'first')
    const later = reduceToBaseline(objectsDump([[7, 'Request']]), 'second')
    const replacement = objectsDump([[7, 'Request']])
    expect(leakCandidates(original, later, replacement)).toHaveLength(1)
  })

  it('refuses baselines without object identities', () => {
    const { objects: _, ...old } = first
    expect(() => leakCandidates(old, second, current)).toThrow(/object identities/)
  })
})

describe('growthSeries', () => {
  it('reports what grows at every step, not what comes and goes', () => {
    const dumps = [
      objectsDump([[1, 'Leak'], [2, 'Noise']]),
      objectsDump([[1, 'Leak'], [3, 'Leak']]),
      objectsDump([[1, 'Leak'], [3, 'Leak'], [4, 'Leak'], [5, 'Noise']]),
    ]
    const baselines = dumps.slice(0, 2).map((dump, k) => reduceToBaseline(dump, `d${k}.json`))
    const series = growthSeries(baselines, dumps[2]!, 'd2.json')

    expect(series.names).toEqual(['d0.json', 'd1.json', 'd2.json'])
    expect(series.classes.map((row) => [row.key, row.counts])).toEqual([['Leak', [1, 2, 3]]])
    expect(series.userClasses).toEqual([10, 10, 10])
  })
})
