import { expect, test } from '@playwright/test'
import { fileURLToPath } from 'node:url'
import { watchCsp } from './helpers'

test('the start page explains the extension and its settings without leaving the page', async ({ page, context, baseURL }) => {
  const origin = new URL(baseURL!).origin
  const external: string[] = []
  context.on('request', (request) => {
    const url = request.url()
    if (url.startsWith('blob:') || url.startsWith('data:')) return
    if (new URL(url).origin !== origin) external.push(url)
  })
  const cspViolations = await watchCsp(page)

  await page.setViewportSize({ width: 1280, height: 720 })
  await page.goto('/')

  await expect(page).toHaveTitle('PHP Meminfo — memory dump analyzer')
  await expect(page.locator('meta[name="description"]')).toHaveAttribute('content', /never uploaded/)

  // The drop zone stays the first thing on screen.
  const open = page.getByRole('button', { name: 'Open dump…' })
  await expect(open).toBeInViewport()

  for (const heading of ['What it is', 'Install the extension', 'Settings', 'Getting a dump']) {
    await expect(page.getByRole('heading', { name: heading, level: 2 })).toBeVisible()
  }

  const settings = page.getByRole('table')
  await expect(settings.getByRole('row', { name: /meminfo\.dump_signal/ })).toContainText('SIGURG')
  await expect(settings.getByRole('row', { name: /meminfo\.dump_dir/ })).toContainText('system temp dir')

  // Details are links to the README, not content fetched into the page.
  const links = page.locator('.guide a')
  expect(await links.count()).toBeGreaterThan(0)
  for (const href of await links.evaluateAll((anchors) => anchors.map((a) => a.getAttribute('href')))) {
    expect(href).toMatch(/^https:\/\/github\.com\/VantaFinance\/php-meminfo(#[a-z-]+)?$/)
  }

  expect(external).toEqual([])
  expect(await cspViolations()).toEqual([])
})

test('the start page fits a phone screen', async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 800 })
  await page.goto('/')

  await expect(page.getByRole('button', { name: 'Open dump…' })).toBeInViewport()
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth)
  expect(overflow).toBe(0)
})


test('demo opens an interactive sample and can be closed', async ({ page, context, baseURL }) => {
  const origin = new URL(baseURL!).origin
  const external: string[] = []
  context.on('request', (request) => {
    const url = request.url()
    if (!url.startsWith('blob:') && !url.startsWith('data:') && new URL(url).origin !== origin) external.push(url)
  })
  const violations = await watchCsp(page)
  await page.goto('/')
  await page.getByRole('button', { name: 'Try demo' }).focus()
  await page.keyboard.press('Enter')
  await expect(page.getByText('Demo data')).toBeVisible()
  await expect(page.getByRole('heading', { name: 'Largest changes' })).toBeVisible()
  await expect(page.locator('.change-chart .link').first()).toHaveCSS('color', 'rgb(47, 91, 211)')
  await page.getByRole('button', { name: /Leak suspects/ }).click()
  await expect(page.getByRole('heading', { name: 'Largest retainers' })).toBeVisible()
  await page.getByRole('button', { name: 'Close', exact: true }).click()
  await expect(page.getByRole('button', { name: 'Try demo' })).toBeVisible()
  expect(external).toEqual([])
  expect(await violations()).toEqual([])
})

test('opening a user dump replaces the demo and its baselines', async ({ page }) => {
  await page.goto('/')
  await page.getByRole('button', { name: 'Try demo' }).click()
  await expect(page.getByText('Demo data')).toBeVisible()
  await page.locator('input[aria-label="Open dump"]').setInputFiles({
    name: 'broken.json', mimeType: 'application/json', buffer: Buffer.from('{'),
  })
  await expect(page.getByRole('alert')).toContainText('broken.json could not be opened')
  await expect(page.getByText('Demo data')).toHaveCount(0)
  await page.locator('input[aria-label="Open dump"]').setInputFiles(fileURLToPath(new URL('../tests/fixtures/parity/leak-after.json', import.meta.url)))
  await expect(page.getByRole('grid', { name: 'Biggest retainers' }).getByRole('row').nth(1)).toContainText('LeakyCache::requests')
  await page.getByRole('button', { name: /Growth/ }).click()
  await expect(page.getByRole('button', { name: 'Choose baseline dump…' })).toBeVisible()
})

test('demo diagrams fit a phone screen', async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 800 })
  await page.goto('/')
  await page.getByRole('button', { name: 'Try demo' }).click()
  await expect(page.getByRole('heading', { name: 'Largest changes' })).toBeVisible()
  expect(await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth)).toBe(0)
  await page.getByRole('button', { name: /Leak suspects/ }).click()
  await expect(page.getByRole('heading', { name: 'Largest retainers' })).toBeVisible()
  expect(await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth)).toBe(0)
})
