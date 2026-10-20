#!/usr/bin/env node
// Static guard for the per-view i18n fragments:
//   1. every English key has a Chinese counterpart (and vice versa);
//   2. every t('…') call in the SPA resolves to a key;
//   3. report keys that are no longer referenced (warning only).
//
// This is intentionally a source scan rather than a runtime check: it runs in
// CI without a browser and catches the common drift (a new label added to one
// locale, or a renamed key used by a view).

import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join, relative } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('..', import.meta.url))
const i18nDir = join(root, 'src', 'i18n')
const baseFile = join(root, 'src', 'i18n.ts')

function keysIn(source) {
  return new Set([...source.matchAll(/'([A-Za-z0-9_.-]+)'\s*:/g)].map((match) => match[1]))
}

function splitLocales(source) {
  const index = source.indexOf('const zh')
  if (index < 0) return null
  return [source.slice(0, index), source.slice(index)]
}

function walk(directory) {
  return readdirSync(directory).flatMap((name) => {
    const path = join(directory, name)
    return statSync(path).isDirectory() ? walk(path) : [path]
  })
}

const en = new Set()
const zh = new Set()
const problems = []

const files = [baseFile, ...readdirSync(i18nDir).filter((name) => name.endsWith('.ts')).map((name) => join(i18nDir, name))]
for (const file of files) {
  const locales = splitLocales(readFileSync(file, 'utf8'))
  if (!locales) {
    problems.push(`${relative(root, file)}: missing "const zh" dictionary`)
    continue
  }
  const enKeys = keysIn(locales[0])
  const zhKeys = keysIn(locales[1])
  enKeys.delete('zh-CN')
  zhKeys.delete('zh-CN')
  for (const key of enKeys) en.add(key)
  for (const key of zhKeys) zh.add(key)
  const missingZh = [...enKeys].filter((key) => !zhKeys.has(key))
  const missingEn = [...zhKeys].filter((key) => !enKeys.has(key))
  if (missingZh.length) problems.push(`${relative(root, file)}: missing zh keys: ${missingZh.join(', ')}`)
  if (missingEn.length) problems.push(`${relative(root, file)}: missing en keys: ${missingEn.join(', ')}`)
}

const used = new Map()
for (const file of walk(join(root, 'src'))) {
  if (!/\.(vue|ts)$/.test(file)) continue
  if (file === baseFile || file.startsWith(i18nDir)) continue
  const source = readFileSync(file, 'utf8')
  for (const match of source.matchAll(/\bt\(\s*'([^']+)'/g)) {
    const key = match[1]
    if (!used.has(key)) used.set(key, new Set())
    used.get(key).add(relative(root, file))
  }
}

for (const [key, sources] of used) {
  if (!en.has(key) || !zh.has(key)) {
    problems.push(`missing key "${key}" used by ${[...sources].sort().join(', ')}`)
  }
}

const dynamicPrefixes = [
  'auth.setup.strength.', 'theme.', 'proxy.status.', 'dashboard.greeting.',
  'help.modules.', 'help.shortcut.', 'settings.modules.',
  'transport.session.state.', 'transport.session.event.',
  'listeners.state.', 'listeners.sessionState.', 'listeners.event.', 'listeners.sessionEvent.',
  'socks5.state.', 'socks5.session.', 'socks5.event.', 'terminal.state.', 'terminal.ws.', 'terminal.category.',
]
const unused = [...en].filter(
  (key) => !used.has(key) && !dynamicPrefixes.some((prefix) => key.startsWith(prefix)),
)

if (problems.length) {
  for (const problem of problems) console.error(`i18n: ${problem}`)
  process.exit(1)
}

console.log(`i18n OK: ${en.size} keys, ${used.size} referenced`)
if (unused.length) {
  console.warn(`i18n warning: ${unused.length} unreferenced keys (dynamic keys are excluded)`)
}
