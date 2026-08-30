/**
 * Model-facing plugin-market tool: lets the agent itself search the plugin
 * ecosystem, list what is installed in the web profile, inspect a package,
 * and install or remove plugins — no manual terminal step needed.
 *
 * Search goes through the npm registry API (fast, reliable in CN networks);
 * install/remove reuses the harness's own `dsh plugin --profile web`
 * command so profile bundle registration, pnpm resolution and lockfile
 * handling stay identical to a manual install. The tool deliberately does
 * not expose arbitrary pnpm arguments — only the four safe actions above.
 *
 * @module @deepseek-ai/dsh-tool-plugin-market
 */

import type { Context } from '@deepseek-ai/cordis'
import { execFile } from 'node:child_process'
import { existsSync, readFileSync } from 'node:fs'
import { homedir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { defineTool } from '@deepseek-ai/dsh-tools'
import { FIRST_PARTY_SECTION_ORDER } from '@deepseek-ai/dsh-system-prompt'

/** Cordis plugin name used by loader diagnostics. */
export const name = 'tool-plugin-market'

/** Services required by this tool. */
export const inject = ['tools', 'systemPrompt']

const PROFILE = 'web'
const NPM_SEARCH_URL = 'https://registry.npmjs.org/-/v1/search'
const NPM_PACKAGE_URL = 'https://registry.npmjs.org'
const SEARCH_TIMEOUT_MS = 12_000
const CLI_TIMEOUT_MS = 180_000
const PACKAGE_PATTERN = /^(?:@[a-z0-9][a-z0-9._~-]*\/)?[a-z0-9][a-z0-9._~-]*$/i

const ACTIONS = ['search', 'list', 'info', 'install', 'remove'] as const

/** The harness home (`~/.dsh` by default). */
function dshHome(): string {
  return process.env.DSH_HOME ?? join(homedir(), '.dsh')
}

/** The web profile directory plugins install into. */
function profileDir(): string {
  return join(dshHome(), 'profiles', PROFILE)
}

/**
 * Locate the harness checkout that owns this tool (the `apps/cli` plugin
 * command lives there). Walks up from the compiled module, then falls back to
 * the server's cwd (the Termux start script cd's to the repo root).
 */
function repoRoot(): string {
  let dir = dirname(fileURLToPath(import.meta.url))
  for (let i = 0; i < 8; i++) {
    if (existsSync(join(dir, 'apps', 'cli', 'lib', 'bin.js'))) return dir
    const parent = dirname(dir)
    if (parent === dir) break
    dir = parent
  }
  const cwd = process.cwd()
  if (existsSync(join(cwd, 'apps', 'cli', 'lib', 'bin.js'))) return cwd
  throw new Error('plugin-market: harness checkout (apps/cli/lib/bin.js) not found')
}

/** Run the harness's own `dsh plugin --profile web` command with args. */
function runCli(args: readonly string[], timeoutMs: number): Promise<{ code: number; output: string }> {
  return new Promise((resolve) => {
    const root = repoRoot()
    execFile(
      process.execPath,
      ['--expose-internals', join(root, 'apps', 'cli', 'lib', 'bin.js'), 'plugin', '--profile', PROFILE, ...args],
      { timeout: timeoutMs, maxBuffer: 4 * 1024 * 1024, env: process.env },
      (error, stdout, stderr) => {
        const output = `${stdout ?? ''}${stderr ? `\n${stderr}` : ''}`.trim()
        resolve({ code: error === null ? 0 : (typeof error.code === 'number' ? error.code : 1), output })
      },
    )
  })
}

/** Read the profile's declared plugin bundles with their installed versions. */
function listInstalled(): Array<{ id: string; version: string }> {
  const packagePath = join(profileDir(), 'package.json')
  if (!existsSync(packagePath)) return []
  let manifest: { dsh?: { profile?: { bundles?: string[] } } }
  try {
    manifest = JSON.parse(readFileSync(packagePath, 'utf8'))
  } catch {
    return []
  }
  const bundles = manifest.dsh?.profile?.bundles ?? []
  return bundles.map((id) => {
    let version = ''
    try {
      const candidate = join(profileDir(), 'node_modules', id, 'package.json')
      if (existsSync(candidate)) {
        version = (JSON.parse(readFileSync(candidate, 'utf8')) as { version?: string }).version ?? ''
      }
    } catch {
      version = ''
    }
    return { id, version }
  })
}

/** Search the npm registry for dsh-related packages matching `query`. */
async function searchNpm(query: string): Promise<string[]> {
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), SEARCH_TIMEOUT_MS)
  try {
    const url = `${NPM_SEARCH_URL}?text=${encodeURIComponent(`${query} dsh`)}&size=20`
    const response = await fetch(url, { signal: controller.signal, headers: { accept: 'application/json' } })
    if (!response.ok) throw new Error(`npm search HTTP ${response.status}`)
    const data = (await response.json()) as {
      objects?: Array<{ package: { name: string; version: string; description?: string; keywords?: string[] } }>
    }
    return (data.objects ?? [])
      .map((entry) => entry.package)
      .filter((pkg) => /dsh/i.test(pkg.name) || (pkg.keywords ?? []).some((keyword) => /dsh|deepseek/i.test(keyword)))
      .slice(0, 15)
      .map((pkg) => `${pkg.name}@${pkg.version}${pkg.description ? ` — ${pkg.description}` : ''}`)
  } finally {
    clearTimeout(timer)
  }
}

/** Resolve one package's latest metadata from the npm registry. */
async function npmPackageInfo(packageName: string): Promise<string> {
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), SEARCH_TIMEOUT_MS)
  try {
    const url = `${NPM_PACKAGE_URL}/${encodeURIComponent(packageName).replace('%40', '@').replace('%2F', '/')}`
    const response = await fetch(url, { signal: controller.signal, headers: { accept: 'application/json' } })
    if (!response.ok) throw new Error(`npm HTTP ${response.status}`)
    const data = (await response.json()) as { name?: string; description?: string; 'dist-tags'?: Record<string, string> }
    return [
      data.name ?? packageName,
      data['dist-tags']?.latest !== undefined ? `latest: ${data['dist-tags'].latest}` : '',
      data.description ?? '',
    ].filter(Boolean).join('\n')
  } finally {
    clearTimeout(timer)
  }
}

/** Reject a malformed package argument before it reaches the CLI. */
function assertPackageName(packageName: string): void {
  if (!PACKAGE_PATTERN.test(packageName)) {
    throw new Error(`plugin-market: invalid package name ${JSON.stringify(packageName)}`)
  }
}

const tool = defineTool({
  name: 'plugin-market',
  description: 'Manage DeepSeek Harness plugins directly: search the plugin ecosystem, list plugins installed in the web profile, '
    + 'inspect a package, and install or remove a plugin. Prefer this tool over shell pnpm for plugin management. '
    + 'Installing a third-party plugin executes its code — confirm with the user before installing anything new.',
  parameters: {
    action: {
      type: 'string',
      required: true,
      enum: [...ACTIONS],
      description: 'search (find plugins by keyword) | list (installed plugins) | info (package details) | install (add a plugin) | remove (uninstall a plugin)',
    },
    query: { type: 'string', description: 'Search keyword, used by action=search.' },
    package: { type: 'string', description: 'npm package name (e.g. dsh-chatvoice or @scope/name), used by info/install/remove.' },
  },
  timeoutMs: CLI_TIMEOUT_MS,
  output: {
    schema: {
      type: 'object',
      additionalProperties: false,
      properties: {
        ok: { type: 'boolean', required: true },
        text: { type: 'string', required: true },
      },
    },
    render: (_args, value) => [{ type: 'text', text: value.text }],
  },
  async execute(args) {
    switch (args.action) {
      case 'search': {
        const query = (args.query ?? '').trim()
        if (query === '') return { ok: false, text: 'plugin-market: action=search requires a query.' }
        try {
          const rows = await searchNpm(query)
          if (rows.length === 0) return { ok: true, text: `No dsh-related packages found for "${query}".` }
          return {
            ok: true,
            text: `Found ${rows.length} dsh-related packages for "${query}":\n${rows.join('\n')}\n\nUse action=install with a package name to install one.`,
          }
        } catch (error) {
          return { ok: false, text: `plugin-market: npm search failed (${String(error)}). Check network and retry.` }
        }
      }
      case 'list': {
        const rows = listInstalled()
        if (rows.length === 0) return { ok: true, text: 'No plugins are installed in the web profile yet.' }
        return {
          ok: true,
          text: `Plugins installed in the web profile (${rows.length}):\n${rows.map((row) => (row.version ? `${row.id}@${row.version}` : row.id)).join('\n')}`,
        }
      }
      case 'info': {
        const packageName = (args.package ?? '').trim()
        if (packageName === '') return { ok: false, text: 'plugin-market: action=info requires a package name.' }
        assertPackageName(packageName)
        try {
          return { ok: true, text: await npmPackageInfo(packageName) }
        } catch (error) {
          return { ok: false, text: `plugin-market: could not resolve ${packageName} (${String(error)}).` }
        }
      }
      case 'install': {
        const packageName = (args.package ?? '').trim()
        if (packageName === '') return { ok: false, text: 'plugin-market: action=install requires a package name.' }
        assertPackageName(packageName)
        const result = await runCli(['add', '-w', packageName], CLI_TIMEOUT_MS)
        if (result.code !== 0) return { ok: false, text: `plugin-market: install of ${packageName} failed:\n${result.output.slice(-1200)}` }
        return {
          ok: true,
          text: `Installed ${packageName} into the web profile.\n${result.output.slice(-600)}\nRestart dsh web (bash docs/termux/start-dsh-web.sh --bg) to activate it.`,
        }
      }
      case 'remove': {
        const packageName = (args.package ?? '').trim()
        if (packageName === '') return { ok: false, text: 'plugin-market: action=remove requires a package name.' }
        assertPackageName(packageName)
        const result = await runCli(['remove', '-w', packageName], CLI_TIMEOUT_MS)
        if (result.code !== 0) return { ok: false, text: `plugin-market: removal of ${packageName} failed:\n${result.output.slice(-1200)}` }
        return {
          ok: true,
          text: `Removed ${packageName} from the web profile.\n${result.output.slice(-600)}\nRestart dsh web to complete the change.`,
        }
      }
      default:
        return { ok: false, text: `plugin-market: unknown action ${JSON.stringify(args.action)}.` }
    }
  },
})

/**
 * Register the plugin-market tool.
 * @param ctx - plugin context; registrations are effects scoped to this plugin.
 */
export async function apply(ctx: Context): Promise<void> {
  ctx.systemPrompt.section({
    name: 'tool:plugin-market',
    order: FIRST_PARTY_SECTION_ORDER.TOOL_JOBS + 100,
    text: 'Use the plugin-market tool — not shell pnpm — to search for, inspect, install and remove DeepSeek Harness plugins. '
      + 'Installing a third-party plugin executes its code: confirm the package with the user before installing.',
  })
  ctx.tools.register(tool)
}
