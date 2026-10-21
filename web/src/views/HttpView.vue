<script setup lang="ts">
import { computed, onActivated, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import Icon from '../components/Icon.vue'
import Socks5ProxyFields from '../components/Socks5ProxyFields.vue'
import { del, errorMessage, get, post, put } from '../api'
import type { HistoryEntry, HttpDownload, HttpHeader, HttpResult } from '../types'
import { useUiStore } from '../stores/ui'
import { useAuthStore } from '../stores/auth'
import { useI18nStore } from '../stores/i18n'
import {
  base64ToBytes,
  buildHtmlDocument,
  bytesToText,
  contentTypeOf,
  previewKindOf,
  type PreviewKind,
} from '../lib/preview'

defineOptions({ name: 'HttpView' })

const ui = useUiStore()
const auth = useAuthStore()
const i18n = useI18nStore()
const t = i18n.t
const method = ref('GET')
const url = ref('https://httpbin.org/anything')
const protocol = ref<'http1' | 'http2' | 'http3'>('http1')
const headers = ref<HttpHeader[]>([
  { name: 'accept', value: 'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8' },
  {
    name: 'user-agent',
    value:
      `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) ` +
      `Chrome/131.0.0.0 Safari/537.36 uvscope/${auth.version}`,
  },
])
const body = ref('')
const bodyType = ref('application/json')
const timeoutMs = ref(20000)
const insecure = ref(false)
const decompress = ref(true)
const bodyMode = ref<'raw' | 'urlencoded' | 'multipart'>('raw')
const urlEncodedFields = ref<Array<{ name: string; value: string }>>([{ name: '', value: '' }])
const multipartFields = ref<Array<{ name: string; value: string; kind: 'text' | 'file'; file: File | null }>>([
  { name: '', value: '', kind: 'text', file: null },
])
const capabilities = ref({ http2: true, http3: false, quic: false, brotli: true })
const curlOpen = ref(false)
const curlText = ref('')
const curlImportOpen = ref(false)
const curlImportText = ref('')
const curlImportError = ref('')
const harInput = ref<HTMLInputElement | null>(null)

interface WorkspaceVariable { key: string; value: string }
interface WorkspaceEnvironment { id: string; name: string; variables: WorkspaceVariable[] }
interface CollectionItem {
  id: string
  name: string
  method: string
  url: string
  headers: HttpHeader[]
  body: string
  bodyType: string
  bodyMode: 'raw' | 'urlencoded' | 'multipart'
}
interface Collection { id: string; name: string; items: CollectionItem[] }
interface Workspace {
  environments: WorkspaceEnvironment[]
  activeEnvironmentId: string
  collections: Collection[]
}
const emptyWorkspace = (): Workspace => ({ environments: [], activeEnvironmentId: '', collections: [] })
const workspace = ref<Workspace>(emptyWorkspace())
const selectedCollectionId = ref('')
const envOpen = ref(false)
const envDraft = ref<WorkspaceEnvironment>({ id: '', name: '', variables: [{ key: '', value: '' }] })
const importOpen = ref(false)
const importText = ref('')
const importError = ref('')
const preScript = ref('')
const postScript = ref('')
const scriptTests = ref<Array<{ name: string; passed: boolean; error?: string }>>([])
const downloading = ref(false)
const downloadState = ref<HttpDownload | null>(null)
let downloadTimer: number | undefined
const proxy = ref({ enabled: false, host: '127.0.0.1', port: 1080, username: '', password: '' })
const sending = ref(false)
const result = ref<HttpResult | null>(null)
const responseTab = ref<'body' | 'preview' | 'headers' | 'timing'>('body')
const allowRemotePreview = ref(false)
const allowScriptsPreview = ref(false)
const previewUrl = ref('')
const previewDocUrl = ref('')
const previewDocId = ref('')
let previewDocSeq = 0
const requestTab = ref<'headers' | 'body' | 'scripts'>('headers')
const advancedOpen = ref(false)
const history = ref<HistoryEntry[]>([])
const historyLoading = ref(false)
const startedAt = ref<number>(0)
const elapsed = ref(0)
let ticker: number | undefined

const methods = ['GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'HEAD', 'OPTIONS', 'TRACE']

const statusTone = computed(() => {
  const status = result.value?.status ?? 0
  if (!result.value?.ok) return 'bad'
  if (status >= 500) return 'bad'
  if (status >= 400) return 'warn'
  if (status >= 300) return 'redirect'
  return 'ok'
})

const responseBody = computed(() => {
  const res = result.value
  if (!res?.ok) return res?.error || ''
  if (res.bodyEncoding === 'base64' && res.bodyBase64) {
    try {
      const binary = atob(res.bodyBase64)
      const bytes = new Uint8Array(binary.length)
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
      return [...bytes.slice(0, 2048)]
        .map((byte, index) => byte.toString(16).padStart(2, '0') + (index % 16 === 15 ? '\n' : ' '))
        .join('')
    } catch {
      return res.bodyBase64
    }
  }
  const text = res.bodyText || ''
  const contentType = res.headers?.find((header) => header.name.toLowerCase() === 'content-type')?.value || ''
  if (contentType.includes('json') || text.trim().startsWith('{') || text.trim().startsWith('[')) {
    try {
      return JSON.stringify(JSON.parse(text), null, 2)
    } catch {
      return text
    }
  }
  return text
})

const responseIsBinary = computed(() => result.value?.bodyEncoding === 'base64')

const contentType = computed(() => contentTypeOf(result.value?.headers))
const previewKind = computed<PreviewKind>(() => previewKindOf(contentType.value))
const previewAvailable = computed(
  () => result.value?.ok === true && previewKind.value !== 'none',
)

const responseBytes = computed<Uint8Array | null>(() => {
  const res = result.value
  if (!res?.ok) return null
  if (res.bodyEncoding === 'base64' && res.bodyBase64) {
    try {
      return base64ToBytes(res.bodyBase64)
    } catch {
      return null
    }
  }
  return new TextEncoder().encode(res.bodyText || '')
})

const htmlSource = computed(() => {
  if (previewKind.value !== 'html') return ''
  const bytes = responseBytes.value
  return bytes ? bytesToText(bytes) : ''
})

const htmlPreviewDocument = computed(() => {
  if (previewKind.value !== 'html') return ''
  return buildHtmlDocument(htmlSource.value, {
    allowRemote: allowRemotePreview.value,
    baseUrl: result.value?.request?.url,
  })
})

function revokePreviewUrl() {
  if (!previewUrl.value) return
  URL.revokeObjectURL(previewUrl.value)
  previewUrl.value = ''
}

function revokeScriptPreview() {
  previewDocSeq += 1
  if (previewDocId.value) {
    void del(`/api/preview/${previewDocId.value}`).catch(() => {
      // The token expires on its own; cleanup is best-effort.
    })
    previewDocId.value = ''
  }
  previewDocUrl.value = ''
}

async function loadServerPreview() {
  revokeScriptPreview()
  const needsServer = allowScriptsPreview.value || allowRemotePreview.value
  if (!needsServer || previewKind.value !== 'html' ||
      responseTab.value !== 'preview' || !htmlSource.value) {
    return
  }
  const seq = ++previewDocSeq
  try {
    const response = await post<{ ok: boolean; id?: string; url?: string; error?: string }>(
      '/api/preview',
      {
        html: htmlSource.value,
        baseUrl: result.value?.request?.url,
        mode: allowScriptsPreview.value ? 'scripts' : 'safe',
      },
    )
    if (seq !== previewDocSeq) {
      if (response.id) void del(`/api/preview/${response.id}`).catch(() => {})
      return
    }
    if (!response.ok || !response.url || !response.id) {
      ui.notify('error', t('http.preview.scriptsFailed'), response.error)
      return
    }
    previewDocId.value = response.id
    previewDocUrl.value = response.url
  } catch (err) {
    ui.toastError(err, t('http.preview.scriptsFailed'))
  }
}

function toggleScripts(event: Event) {
  const input = event.target as HTMLInputElement
  if (input.checked && !window.confirm(t('http.preview.scriptsWarning'))) {
    input.checked = false
    return
  }
  allowScriptsPreview.value = input.checked
  if (input.checked) allowRemotePreview.value = true
}

watch(
  [htmlSource, allowScriptsPreview, allowRemotePreview, responseTab],
  () => void loadServerPreview(),
)

watch(
  [() => result.value, previewKind, responseTab],
  () => {
    revokePreviewUrl()
    if (responseTab.value !== 'preview') return
    const kind = previewKind.value
    if (kind === 'none' || kind === 'html') return
    const bytes = responseBytes.value
    if (!bytes) return
    const type = contentType.value.split(';', 1)[0].trim() || 'application/octet-stream'
    const copy = new Uint8Array(bytes)
    previewUrl.value = URL.createObjectURL(new Blob([copy], { type }))
  },
  { immediate: true },
)

watch(previewKind, (kind) => {
  if (kind === 'none' && responseTab.value === 'preview') responseTab.value = 'body'
})

function addHeader() {
  headers.value.push({ name: '', value: '' })
}
function removeHeader(index: number) {
  headers.value.splice(index, 1)
}
function cleanHeaders() {
  return headers.value.filter((header) => header.name.trim())
}

function downloadBody() {
  const res = result.value
  if (!res) return
  // The probe clips at 4 MiB; ask the daemon to re-fetch and stream the full
  // response to disk instead of handing the browser a truncated blob.
  if (res.bodyTruncated) {
    void startStreamDownload()
    return
  }
  let blob: Blob
  if (res.bodyEncoding === 'base64' && res.bodyBase64) {
    const binary = atob(res.bodyBase64)
    const bytes = new Uint8Array(binary.length)
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
    blob = new Blob([bytes])
  } else {
    blob = new Blob([res.bodyText || ''], { type: 'text/plain' })
  }
  const anchor = document.createElement('a')
  anchor.href = URL.createObjectURL(blob)
  anchor.download = `uvscope-response-${Date.now()}.bin`
  anchor.click()
  window.setTimeout(() => URL.revokeObjectURL(anchor.href), 1000)
}

async function copy(text: string) {
  try {
    await navigator.clipboard.writeText(text)
    ui.notify('success', t('http.notify.copied'))
  } catch {
    ui.notify('error', t('http.notify.clipboardDenied'))
  }
}

function addUrlEncodedField() {
  urlEncodedFields.value.push({ name: '', value: '' })
}

function removeUrlEncodedField(index: number) {
  urlEncodedFields.value.splice(index, 1)
}

function addMultipartField() {
  multipartFields.value.push({ name: '', value: '', kind: 'text', file: null })
}

function removeMultipartField(index: number) {
  multipartFields.value.splice(index, 1)
}

function pickMultipartFile(field: { file: File | null }, event: Event) {
  field.file = (event.target as HTMLInputElement).files?.[0] ?? null
}

function multipartName(value: string): string {
  return value.replace(/["\r\n]/g, '_')
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = ''
  const chunk = 0x8000
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk))
  }
  return btoa(binary)
}

function shellQuote(value: string): string {
  return `'${value.replace(/'/g, `'\\''`)}'`
}

function tokenizeCurl(input: string): string[] {
  const tokens: string[] = []
  let current = ''
  let quote: '"' | "'" | null = null
  let escaped = false
  const text = input.replace(/\\\r?\n/g, ' ')
  for (const c of text) {
    if (escaped) {
      current += c
      escaped = false
      continue
    }
    if (c === '\\' && quote !== "'") {
      escaped = true
      continue
    }
    if (quote) {
      if (c === quote) quote = null
      else current += c
      continue
    }
    if (c === "'" || c === '"') {
      quote = c
      continue
    }
    if (/\s/.test(c)) {
      if (current) {
        tokens.push(current)
        current = ''
      }
      continue
    }
    current += c
  }
  if (current) tokens.push(current)
  return tokens
}

function buildCurlCommand(): string {
  const outgoing = cleanHeaders()
  const hasContentType = outgoing.some((header) => header.name.toLowerCase() === 'content-type')
  const lines: string[] = [`curl -X ${method.value} ${shellQuote(url.value.trim())}`]
  let bodyArgument = ''
  if (bodyMode.value === 'multipart') {
    for (const field of multipartFields.value) {
      const name = field.name.trim()
      if (!name) continue
      if (field.kind === 'file' && field.file) {
        lines.push(`  -F ${shellQuote(`${name}=@${field.file.name}`)}`)
      } else if (field.kind === 'text') {
        lines.push(`  -F ${shellQuote(`${name}=${field.value}`)}`)
      }
    }
  } else {
    let encoded = body.value
    if (bodyMode.value === 'urlencoded') {
      const params = new URLSearchParams()
      for (const field of urlEncodedFields.value) {
        if (field.name.trim()) params.append(field.name.trim(), field.value)
      }
      encoded = params.toString()
    }
    if (encoded) bodyArgument = encoded
  }
  for (const header of outgoing) {
    lines.push(`  -H ${shellQuote(`${header.name}: ${header.value}`)}`)
  }
  if (bodyMode.value === 'urlencoded' && !hasContentType) {
    lines.push(`  -H ${shellQuote('content-type: application/x-www-form-urlencoded')}`)
  } else if (bodyMode.value === 'raw' && bodyArgument && !hasContentType) {
    lines.push(`  -H ${shellQuote(`content-type: ${bodyType.value}`)}`)
  }
  if (bodyArgument) lines.push(`  --data-binary ${shellQuote(bodyArgument)}`)
  if (insecure.value) lines.push('  -k')
  if (proxy.value.enabled) {
    const auth = proxy.value.username
      ? `${proxy.value.username}:${proxy.value.password}@`
      : ''
    lines.push(`  --socks5-hostname ${shellQuote(`${auth}${proxy.value.host.trim()}:${proxy.value.port}`)}`)
  }
  return lines.join(' \\\n')
}

function exportHar() {
  const res = result.value
  if (!res?.ok) {
    ui.notify('warning', t('http.har.noResponse'))
    return
  }
  const requestHeaders = res.request
    ? cleanHeaders().map((header) => ({ name: header.name, value: header.value }))
    : []
  const responseHeaders = (res.headers || []).map((header) => ({ name: header.name, value: header.value }))
  const started = res.startedAt || new Date().toISOString()
  const har = {
    log: {
      version: '1.2',
      creator: { name: 'uvscope', version: auth.version },
      entries: [
        {
          startedDateTime: started,
          time: res.timing?.totalMs ?? 0,
          request: {
            method: res.request?.method || method.value,
            url: res.request?.url || url.value,
            httpVersion: res.httpVersion || 'HTTP/1.1',
            cookies: [],
            headers: requestHeaders,
            queryString: [],
            headersSize: -1,
            bodySize: res.bodySize ?? 0,
          },
          response: {
            status: res.status ?? 0,
            statusText: res.statusText || '',
            httpVersion: res.httpVersion || 'HTTP/1.1',
            cookies: [],
            headers: responseHeaders,
            content: {
              size: res.bodySize ?? 0,
              mimeType: contentType.value.split(';', 1)[0] || 'application/octet-stream',
              text: res.bodyText || '',
              encoding: res.bodyEncoding === 'base64' ? 'base64' : undefined,
            },
            redirectURL: '',
            headersSize: -1,
            bodySize: res.bodySize ?? 0,
          },
          cache: {},
          timings: { send: 0, wait: res.timing?.totalMs ?? 0, receive: 0 },
        },
      ],
    },
  }
  const blob = new Blob([JSON.stringify(har, null, 2)], { type: 'application/json' })
  const anchor = document.createElement('a')
  anchor.href = URL.createObjectURL(blob)
  anchor.download = `uvscope-${Date.now()}.har`
  anchor.click()
  window.setTimeout(() => URL.revokeObjectURL(anchor.href), 1000)
}

function pickHarFile() {
  harInput.value?.click()
}

async function importHar(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (!file) return
  try {
    const parsed = JSON.parse(await file.text())
    const entries = Array.isArray(parsed?.log?.entries) ? parsed.log.entries : []
    if (!entries.length) throw new Error(t('http.har.empty'))
    const request = entries[0].request || {}
    if (typeof request.url === 'string') url.value = request.url
    if (typeof request.method === 'string') method.value = request.method.toUpperCase()
    const harHeaders = Array.isArray(request.headers) ? request.headers : []
    if (harHeaders.length) {
      headers.value = harHeaders
        .filter((header: Record<string, unknown>) => header && header.name)
        .map((header: Record<string, unknown>) => ({
          name: String(header.name),
          value: String(header.value ?? ''),
        }))
    }
    const post = request.postData
    bodyMode.value = 'raw'
    body.value = typeof post?.text === 'string' ? post.text : ''
    const contentTypeHeader = harHeaders.find(
      (header: Record<string, unknown>) => String(header.name).toLowerCase() === 'content-type',
    )
    const mime = String(post?.mimeType || contentTypeHeader?.value || '')
    if (/json/i.test(mime)) bodyType.value = 'application/json'
    else if (/xml/i.test(mime)) bodyType.value = 'application/xml'
    else if (/x-www-form-urlencoded/i.test(mime)) bodyType.value = 'application/x-www-form-urlencoded'
    else if (/octet-stream/i.test(mime)) bodyType.value = 'application/octet-stream'
    else if (mime) bodyType.value = 'text/plain'
    ui.notify('success', t('http.har.imported', { count: entries.length }))
  } catch (err) {
    ui.toastError(err, t('http.har.importFailed'))
  }
}

function openCurlExport() {
  curlText.value = buildCurlCommand()
  curlOpen.value = true
}

async function copyCurl() {
  try {
    await navigator.clipboard.writeText(curlText.value)
    ui.notify('success', t('http.notify.copied'))
  } catch {
    ui.notify('error', t('http.notify.clipboardDenied'))
  }
}

function applyCurlImport() {
  curlImportError.value = ''
  const input = curlImportText.value.trim()
  if (!input) return
  const tokens = tokenizeCurl(input)
  let target = ''
  let parsedMethod = ''
  const parsedHeaders: HttpHeader[] = []
  const dataParts: string[] = []
  const formFields: Array<{ name: string; value: string; kind: 'text' | 'file'; file: File | null }> = []
  let insecureValue = false
  let proxyValue: { host: string; port: number; username: string; password: string } | null = null
  const take = (index: number) => (index + 1 < tokens.length ? tokens[index + 1] : '')
  for (let i = 0; i < tokens.length; i += 1) {
    const token = tokens[i]
    if (token === 'curl') continue
    switch (token) {
      case '-X':
      case '--request':
        parsedMethod = take(i).toUpperCase()
        i += 1
        break
      case '-H':
      case '--header': {
        const header = take(i)
        const colon = header.indexOf(':')
        if (colon > 0) {
          parsedHeaders.push({ name: header.slice(0, colon).trim(), value: header.slice(colon + 1).trim() })
        }
        i += 1
        break
      }
      case '-d':
      case '--data':
      case '--data-raw':
      case '--data-binary':
        dataParts.push(take(i))
        i += 1
        break
      case '--data-urlencode': {
        const value = take(i)
        dataParts.push(value.includes('=') ? value : `${value}=`)
        i += 1
        break
      }
      case '--url':
        target = take(i)
        i += 1
        break
      case '-k':
      case '--insecure':
        insecureValue = true
        break
      case '-u':
      case '--user': {
        const credentials = take(i)
        const colon = credentials.indexOf(':')
        const pair = colon >= 0 ? credentials : `${credentials}:`
        parsedHeaders.push({ name: 'authorization', value: `Basic ${btoa(pair)}` })
        i += 1
        break
      }
      case '-b':
      case '--cookie':
        parsedHeaders.push({ name: 'cookie', value: take(i) })
        i += 1
        break
      case '-A':
      case '--user-agent':
        parsedHeaders.push({ name: 'user-agent', value: take(i) })
        i += 1
        break
      case '-e':
      case '--referer':
        parsedHeaders.push({ name: 'referer', value: take(i) })
        i += 1
        break
      case '--socks5':
      case '--socks5-hostname':
      case '--socks5h': {
        let value = take(i)
        let username = ''
        let password = ''
        const at = value.lastIndexOf('@')
        if (at > 0) {
          const auth = value.slice(0, at)
          value = value.slice(at + 1)
          const colon = auth.indexOf(':')
          username = colon >= 0 ? auth.slice(0, colon) : auth
          password = colon >= 0 ? auth.slice(colon + 1) : ''
        }
        const colon = value.lastIndexOf(':')
        if (colon > 0) {
          proxyValue = {
            host: value.slice(0, colon),
            port: Number(value.slice(colon + 1)) || 1080,
            username,
            password,
          }
        }
        i += 1
        break
      }
      case '-F':
      case '--form': {
        const value = take(i)
        const eq = value.indexOf('=')
        if (eq > 0) {
          const name = value.slice(0, eq)
          const rest = value.slice(eq + 1)
          if (rest.startsWith('@')) {
            formFields.push({ name, value: '', kind: 'file', file: null })
          } else {
            formFields.push({ name, value: rest, kind: 'text', file: null })
          }
        }
        i += 1
        break
      }
      case '-I':
      case '--head':
        parsedMethod = 'HEAD'
        break
      case '--compressed':
      case '-G':
      case '--get':
        break
      default:
        if (!token.startsWith('-') && !target) target = token
        break
    }
  }
  if (!target) {
    curlImportError.value = t('http.curl.noUrl')
    return
  }
  const rawBody = dataParts.join('&')
  url.value = target
  method.value = parsedMethod || (rawBody ? 'POST' : 'GET')
  if (parsedHeaders.length) headers.value = parsedHeaders
  const contentType = parsedHeaders.find((header) => header.name.toLowerCase() === 'content-type')?.value || ''
  if (formFields.length) {
    bodyMode.value = 'multipart'
    multipartFields.value = formFields
    body.value = ''
  } else {
    bodyMode.value = 'raw'
    body.value = rawBody
  }
  if (/json/i.test(contentType)) bodyType.value = 'application/json'
  else if (/xml/i.test(contentType)) bodyType.value = 'application/xml'
  else if (/x-www-form-urlencoded/i.test(contentType)) bodyType.value = 'application/x-www-form-urlencoded'
  else if (/octet-stream/i.test(contentType)) bodyType.value = 'application/octet-stream'
  else if (contentType) bodyType.value = 'text/plain'
  insecure.value = insecureValue
  if (proxyValue) {
    proxy.value = { enabled: true, ...proxyValue }
  }
  curlImportOpen.value = false
  curlImportText.value = ''
  ui.notify('success', t('http.curl.imported'))
}

function newId(): string {
  return `${Date.now().toString(16)}${Math.random().toString(16).slice(2, 10)}`
}

function activeVariables(): Record<string, string> {
  const environment = workspace.value.environments.find(
    (item) => item.id === workspace.value.activeEnvironmentId,
  )
  const map: Record<string, string> = {}
  for (const variable of environment?.variables || []) {
    if (variable.key) map[variable.key] = variable.value
  }
  return map
}

function substitute(value: string): string {
  const variables = activeVariables()
  return value.replace(/\{\{\s*([\w.-]+)\s*\}\}/g, (match, name: string) =>
    Object.prototype.hasOwnProperty.call(variables, name) ? variables[name] : match,
  )
}

async function loadWorkspace() {
  try {
    const response = await get<{ workspace?: Workspace }>('/api/http/workspace')
    if (response.workspace) {
      workspace.value = {
        environments: response.workspace.environments || [],
        activeEnvironmentId: response.workspace.activeEnvironmentId || '',
        collections: response.workspace.collections || [],
      }
    }
  } catch {
    // Workspaces are optional; the request editor still works without one.
  }
}

async function saveWorkspace() {
  try {
    await put('/api/http/workspace', workspace.value)
  } catch (err) {
    ui.toastError(err, t('http.workspace.saveFailed'))
  }
}

function openEnvironment() {
  envDraft.value = { id: '', name: '', variables: [{ key: '', value: '' }] }
  envOpen.value = true
}

function editEnvironment(environment: WorkspaceEnvironment) {
  envDraft.value = {
    id: environment.id,
    name: environment.name,
    variables: environment.variables.length
      ? environment.variables.map((variable) => ({ ...variable }))
      : [{ key: '', value: '' }],
  }
}

function addWorkspaceVariable() {
  envDraft.value.variables.push({ key: '', value: '' })
}

function removeWorkspaceVariable(index: number) {
  envDraft.value.variables.splice(index, 1)
}

async function saveEnvironment() {
  if (!envDraft.value.name.trim()) {
    ui.notify('warning', t('http.workspace.nameRequired'))
    return
  }
  const variables = envDraft.value.variables
    .filter((variable) => variable.key.trim())
    .map((variable) => ({ key: variable.key.trim(), value: variable.value }))
  const existing = workspace.value.environments.find((item) => item.id === envDraft.value.id)
  if (existing) {
    existing.name = envDraft.value.name.trim()
    existing.variables = variables
  } else {
    const environment: WorkspaceEnvironment = {
      id: envDraft.value.id || newId(),
      name: envDraft.value.name.trim(),
      variables,
    }
    workspace.value.environments.push(environment)
    workspace.value.activeEnvironmentId = environment.id
  }
  await saveWorkspace()
  envDraft.value = { id: '', name: '', variables: [{ key: '', value: '' }] }
}

async function deleteEnvironment(id: string) {
  if (!window.confirm(t('http.workspace.deleteEnvConfirm'))) return
  workspace.value.environments = workspace.value.environments.filter((item) => item.id !== id)
  if (workspace.value.activeEnvironmentId === id) workspace.value.activeEnvironmentId = ''
  if (envDraft.value.id === id) envDraft.value = { id: '', name: '', variables: [{ key: '', value: '' }] }
  await saveWorkspace()
}

function selectedCollection(): Collection | undefined {
  return workspace.value.collections.find((item) => item.id === selectedCollectionId.value)
}

async function saveToCollection() {
  let collection = selectedCollection()
  if (!collection) {
    const name = window.prompt(t('http.workspace.newCollectionName'))
    if (!name?.trim()) return
    collection = { id: newId(), name: name.trim(), items: [] }
    workspace.value.collections.push(collection)
    selectedCollectionId.value = collection.id
  }
  const itemName = window.prompt(t('http.workspace.itemName'), `${method.value} ${url.value}`)
  if (!itemName?.trim()) return
  collection.items.push({
    id: newId(),
    name: itemName.trim(),
    method: method.value,
    url: url.value,
    headers: cleanHeaders(),
    body: body.value,
    bodyType: bodyType.value,
    bodyMode: bodyMode.value,
  })
  await saveWorkspace()
  ui.notify('success', t('http.workspace.saved'))
}

function loadCollectionItem(item: CollectionItem) {
  url.value = item.url
  method.value = item.method
  headers.value = item.headers?.length ? item.headers.map((header) => ({ ...header })) : headers.value
  body.value = item.body || ''
  bodyType.value = item.bodyType || 'application/json'
  bodyMode.value = item.bodyMode || 'raw'
}

async function importCollectionFile(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (!file) return
  importText.value = await file.text()
  applyImport()
}

function postmanItems(items: unknown[], collection: Collection, prefix = '') {
  for (const raw of items) {
    if (!raw || typeof raw !== 'object') continue
    const entry = raw as Record<string, unknown>
    const name = String(entry.name || 'request')
    const request = entry.request as Record<string, unknown> | undefined
    if (request) {
      let url = ''
      if (typeof request.url === 'string') url = request.url
      else if (request.url && typeof request.url === 'object') {
        const rawUrl = (request.url as Record<string, unknown>).raw
        url = typeof rawUrl === 'string' ? rawUrl : ''
      }
      const headers: HttpHeader[] = []
      const rawHeaders = request.header
      if (Array.isArray(rawHeaders)) {
        for (const header of rawHeaders) {
          if (header && typeof header === 'object') {
            const item = header as Record<string, unknown>
            if (item.key && !item.disabled) {
              headers.push({ name: String(item.key), value: String(item.value ?? '') })
            }
          }
        }
      }
      let bodyText = ''
      const body = request.body as Record<string, unknown> | undefined
      if (body && typeof body.raw === 'string') bodyText = body.raw
      collection.items.push({
        id: newId(),
        name: `${prefix}${name}`,
        method: String(request.method || 'GET').toUpperCase(),
        url,
        headers,
        body: bodyText,
        bodyType: 'application/json',
        bodyMode: 'raw',
      })
    }
    if (Array.isArray(entry.item)) postmanItems(entry.item, collection, `${prefix}${name} / `)
  }
}

function applyImport() {
  importError.value = ''
  try {
    const data = JSON.parse(importText.value)
    const collection: Collection = { id: newId(), name: 'Imported', items: [] }
    if (data && typeof data === 'object' && Array.isArray((data as Record<string, unknown>).item)) {
      const info = (data as Record<string, unknown>).info as Record<string, unknown> | undefined
      collection.name = String(info?.name || 'Postman import')
      postmanItems((data as Record<string, unknown>).item as unknown[], collection)
    } else if (data && typeof data === 'object' &&
               (((data as Record<string, unknown>).openapi) || ((data as Record<string, unknown>).swagger))) {
      const spec = data as Record<string, unknown>
      const info = spec.info as Record<string, unknown> | undefined
      collection.name = String(info?.title || 'OpenAPI import')
      let base = ''
      const servers = spec.servers
      if (Array.isArray(servers) && servers[0] && typeof servers[0] === 'object') {
        base = String((servers[0] as Record<string, unknown>).url || '')
      } else if (spec.host) {
        const scheme = Array.isArray(spec.schemes) && spec.schemes[0] ? String(spec.schemes[0]) : 'https'
        base = `${scheme}://${String(spec.host)}${String(spec.basePath || '')}`
      }
      const paths = spec.paths as Record<string, unknown> | undefined
      for (const [path, operations] of Object.entries(paths || {})) {
        if (!operations || typeof operations !== 'object') continue
        for (const [method, operation] of Object.entries(operations as Record<string, unknown>)) {
          if (!['get', 'post', 'put', 'patch', 'delete', 'head', 'options'].includes(method)) continue
          const op = (operation || {}) as Record<string, unknown>
          const headers: HttpHeader[] = []
          const parameters = op.parameters
          if (Array.isArray(parameters)) {
            for (const parameter of parameters) {
              if (parameter && typeof parameter === 'object') {
                const param = parameter as Record<string, unknown>
                if (param.in === 'header' && param.name) {
                  headers.push({ name: String(param.name), value: String(param.example ?? '') })
                }
              }
            }
          }
          collection.items.push({
            id: newId(),
            name: String(op.summary || op.operationId || `${method.toUpperCase()} ${path}`),
            method: method.toUpperCase(),
            url: `${base}${path}`,
            headers,
            body: '',
            bodyType: 'application/json',
            bodyMode: 'raw',
          })
        }
      }
    }
    if (!collection.items.length) throw new Error(t('http.import.empty'))
    workspace.value.collections.push(collection)
    selectedCollectionId.value = collection.id
    void saveWorkspace()
    importOpen.value = false
    importText.value = ''
    ui.notify('success', t('http.import.imported', { count: collection.items.length }))
  } catch (err) {
    importError.value = err instanceof Error ? err.message : String(err)
  }
}

function activeEnvironment(): WorkspaceEnvironment | undefined {
  return workspace.value.environments.find(
    (item) => item.id === workspace.value.activeEnvironmentId,
  )
}

function setEnvironmentVariable(key: string, value: string) {
  let environment = activeEnvironment()
  if (!environment) {
    environment = { id: newId(), name: 'Script', variables: [] }
    workspace.value.environments.push(environment)
    workspace.value.activeEnvironmentId = environment.id
  }
  const existing = environment.variables.find((variable) => variable.key === key)
  if (existing) existing.value = value
  else environment.variables.push({ key, value })
  void saveWorkspace()
}

async function runPreScript(): Promise<boolean> {
  scriptTests.value = []
  if (!preScript.value.trim()) return true
  const request = {
    method: method.value,
    url: url.value,
    headers: headers.value.map((header) => ({ ...header })),
    body: body.value,
  }
  const api = {
    request,
    env: { get: (key: string) => activeVariables()[key], set: setEnvironmentVariable },
    variables: activeVariables(),
  }
  try {
    const fn = new Function(
      'uvscope',
      `"use strict"; return (async () => {\n${preScript.value}\n})()`,
    )
    await fn(api)
  } catch (err) {
    ui.notify('error', t('http.scripts.preFailed'), err instanceof Error ? err.message : String(err))
    return false
  }
  method.value = request.method
  url.value = request.url
  headers.value = request.headers
  body.value = request.body
  return true
}

async function runPostScript(res: HttpResult) {
  if (!postScript.value.trim() || !res.ok) return
  const contentTypeHeader = (res.headers || []).find(
    (header) => header.name.toLowerCase() === 'content-type',
  )?.value || ''
  let parsedBody: unknown = null
  const parseBody = () => {
    if (parsedBody !== null) return parsedBody
    parsedBody = JSON.parse(res.bodyText || 'null')
    return parsedBody
  }
  const api = {
    response: {
      status: res.status,
      headers: res.headers || [],
      text: res.bodyText || '',
      contentType: contentTypeHeader,
      json: parseBody,
    },
    test: (name: string, fn: () => void) => {
      try {
        fn()
        scriptTests.value.push({ name, passed: true })
      } catch (err) {
        scriptTests.value.push({
          name,
          passed: false,
          error: err instanceof Error ? err.message : String(err),
        })
      }
    },
    env: { get: (key: string) => activeVariables()[key], set: setEnvironmentVariable },
    variables: activeVariables(),
  }
  try {
    const fn = new Function(
      'uvscope',
      `"use strict"; return (async () => {\n${postScript.value}\n})()`,
    )
    await fn(api)
  } catch (err) {
    ui.notify('error', t('http.scripts.postFailed'), err instanceof Error ? err.message : String(err))
  }
}

async function buildPayload() {
  const outgoing = cleanHeaders()
  let requestBody = substitute(body.value)
  let bodyEncoding = 'text'
  let derivedContentType = ''
  if (bodyMode.value === 'urlencoded') {
    const params = new URLSearchParams()
    for (const field of urlEncodedFields.value) {
      if (field.name.trim()) params.append(field.name.trim(), substitute(field.value))
    }
    requestBody = params.toString()
    derivedContentType = 'application/x-www-form-urlencoded'
  } else if (bodyMode.value === 'multipart') {
    const boundary = `----uvscope${Date.now().toString(16)}${Math.random().toString(16).slice(2)}`
    const parts: BlobPart[] = []
    for (const field of multipartFields.value) {
      const name = field.name.trim()
      if (!name) continue
      if (field.kind === 'file' && field.file) {
        parts.push(
          `--${boundary}\r\nContent-Disposition: form-data; name="${multipartName(name)}"; ` +
            `filename="${multipartName(field.file.name)}"\r\n` +
            `Content-Type: ${field.file.type || 'application/octet-stream'}\r\n\r\n`,
        )
        parts.push(field.file)
        parts.push('\r\n')
      } else if (field.kind === 'text') {
        parts.push(
          `--${boundary}\r\nContent-Disposition: form-data; name="${multipartName(name)}"\r\n\r\n` +
            `${substitute(field.value)}\r\n`,
        )
      }
    }
    parts.push(`--${boundary}--\r\n`)
    const blob = new Blob(parts)
    if (blob.size > 8 * 1024 * 1024) throw new Error(t('http.body.tooLarge'))
    requestBody = bytesToBase64(new Uint8Array(await blob.arrayBuffer()))
    bodyEncoding = 'base64'
    derivedContentType = `multipart/form-data; boundary=${boundary}`
  }
  const contentTypeIndex = outgoing.findIndex(
    (header) => header.name.toLowerCase() === 'content-type',
  )
  if (derivedContentType) {
    if (contentTypeIndex >= 0) outgoing.splice(contentTypeIndex, 1)
    outgoing.push({ name: 'content-type', value: derivedContentType })
  } else if (requestBody && contentTypeIndex < 0) {
    outgoing.push({ name: 'content-type', value: bodyType.value })
  }
  for (const header of outgoing) header.value = substitute(header.value)
  return {
    url: substitute(url.value.trim()),
    method: method.value,
    protocol: protocol.value,
    headers: outgoing,
    body: requestBody,
    bodyEncoding,
    timeoutMs: timeoutMs.value,
    insecure: insecure.value,
    decompress: decompress.value,
    proxy: proxy.value.enabled
      ? {
          host: proxy.value.host.trim(),
          port: proxy.value.port,
          username: proxy.value.username,
          password: proxy.value.password,
        }
      : undefined,
  }
}

async function send() {
  if (!url.value.trim()) {
    ui.notify('warning', t('http.notify.urlRequired'))
    return
  }
  if (sending.value) return
  sending.value = true
  result.value = null
  startedAt.value = performance.now()
  elapsed.value = 0
  ticker = window.setInterval(() => {
    elapsed.value = Math.round(performance.now() - startedAt.value)
  }, 100)
  try {
    if (!(await runPreScript())) {
      sending.value = false
      return
    }
    result.value = await post<HttpResult>('/api/http/send', await buildPayload())
    await runPostScript(result.value)
    responseTab.value = 'body'
    await loadHistory()
    if (!result.value.ok) ui.notify('error', t('http.requestFailed'), result.value.error)
  } catch (err) {
    result.value = { ok: false, error: errorMessage(err) }
    ui.toastError(err, t('http.requestFailed'))
  } finally {
    sending.value = false
    if (ticker) window.clearInterval(ticker)
    elapsed.value = Math.round(performance.now() - startedAt.value)
  }
}

function stopDownloadPolling() {
  if (downloadTimer) window.clearInterval(downloadTimer)
  downloadTimer = undefined
}

function triggerBrowserDownload(url: string) {
  const anchor = document.createElement('a')
  anchor.href = url
  anchor.download = ''
  document.body.appendChild(anchor)
  anchor.click()
  anchor.remove()
}

function pollDownload(id: string) {
  stopDownloadPolling()
  downloadTimer = window.setInterval(async () => {
    try {
      const response = await get<{ ok: boolean; download?: HttpDownload }>(
        `/api/http/download/${id}`,
      )
      const state = response.download
      if (!state) return
      downloadState.value = state
      if (state.state === 'ready') {
        stopDownloadPolling()
        downloading.value = false
        triggerBrowserDownload(state.url)
        ui.notify('success', t('http.download.ready'), state.filename)
      } else if (state.state === 'error' || state.state === 'cancelled') {
        stopDownloadPolling()
        downloading.value = false
        if (state.state === 'error') {
          ui.notify('error', t('http.download.failed'), state.error)
        }
      }
    } catch {
      // Keep polling; the daemon may be busy or restarting.
    }
  }, 400)
}

async function startStreamDownload() {
  if (downloading.value || !url.value.trim()) return
  downloading.value = true
  downloadState.value = {
    id: '',
    state: 'running',
    url: '',
    bytes: 0,
    totalBytes: -1,
  }
  try {
    const response = await post<{ ok: boolean; error?: string; download?: HttpDownload }>(
      '/api/http/download',
      await buildPayload(),
    )
    if (!response.ok || !response.download?.id) {
      throw new Error(response.error || t('http.download.failed'))
    }
    downloadState.value = response.download
    pollDownload(response.download.id)
  } catch (err) {
    downloading.value = false
    downloadState.value = null
    ui.toastError(err, t('http.download.failed'))
  }
}

async function cancelDownload() {
  const id = downloadState.value?.id
  stopDownloadPolling()
  downloading.value = false
  downloadState.value = null
  if (!id) return
  await del(`/api/http/download/${id}`).catch(() => {
    // The job expires on its own; cancellation is best-effort.
  })
}

async function loadHistory() {
  historyLoading.value = true
  try {
    const response = await get<{ entries: HistoryEntry[] }>('/api/history?kind=http&limit=30')
    history.value = response.entries
  } catch {
    // History is supplementary; the workbench stays usable if it fails.
  } finally {
    historyLoading.value = false
  }
}

function restore(entry: HistoryEntry) {
  const request = (entry.detail?.request ?? {}) as Record<string, unknown>
  if (typeof request.url === 'string') url.value = request.url
  if (typeof request.method === 'string') method.value = request.method
  const preference = request.protocolPreference
  if (preference === 'http2' || preference === 'http3' || preference === 'http1') protocol.value = preference
}

async function clearHistory() {
  if (!window.confirm(t('http.history.clearConfirm'))) return
  try {
    await del('/api/history?kind=http')
    history.value = []
    ui.notify('success', t('http.history.cleared'))
  } catch (err) {
    ui.toastError(err, t('http.somethingWentWrong'))
  }
}

function formatBytes(bytes: number) {
  if (!bytes) return '0 B'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`
}

function formatTime(value: string) {
  return new Date(value).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

function onKeydown(event: KeyboardEvent) {
  if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
    event.preventDefault()
    void send()
  }
}

watch([preScript, postScript], () => {
  window.localStorage.setItem('uvscope-http-pre-script', preScript.value)
  window.localStorage.setItem('uvscope-http-post-script', postScript.value)
})

async function loadCapabilities() {
  try {
    const health = await get<{ capabilities?: typeof capabilities.value }>('/api/health')
    if (health.capabilities) capabilities.value = health.capabilities
  } catch {
    // Keep the conservative defaults; the API still reports unsupported builds.
  }
}

onMounted(() => {
  window.addEventListener('keydown', onKeydown)
  void loadHistory()
  void loadCapabilities()
  void loadWorkspace()
  preScript.value = window.localStorage.getItem('uvscope-http-pre-script') || ''
  postScript.value = window.localStorage.getItem('uvscope-http-post-script') || ''
})
onActivated(() => void loadHistory())
onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKeydown)
  if (ticker) window.clearInterval(ticker)
  stopDownloadPolling()
  revokePreviewUrl()
  revokeScriptPreview()
})
</script>

<template>
  <div class="page http-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('http.eyebrow') }}</span>
        <h2>{{ t('http.title') }}</h2>
        <p>{{ t('http.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <button class="button button--ghost" @click="openCurlExport"><Icon name="copy" :size="14" />{{ t('http.curl.export') }}</button>
        <button class="button button--ghost" @click="curlImportOpen = true; curlImportError = ''"><Icon name="download" :size="14" />{{ t('http.curl.import') }}</button>
        <button class="button button--ghost" @click="exportHar"><Icon name="download" :size="14" />HAR</button>
        <button class="button button--ghost" @click="pickHarFile"><Icon name="upload" :size="14" />{{ t('http.har.import') }}</button>
        <input ref="harInput" type="file" accept=".har,application/json" class="hidden-file" @change="importHar" />
        <span class="chip mono"><Icon name="terminal" :size="13" /> Ctrl + Enter</span>
      </div>
    </header>

    <div class="workbench">
      <div class="workbench__main">
        <section class="request-card">
          <div class="workspace-bar">
            <label class="workspace-field">
              <span>{{ t('http.workspace.environment') }}</span>
              <select v-model="workspace.activeEnvironmentId" class="select select--compact" @change="saveWorkspace">
                <option value="">{{ t('http.workspace.none') }}</option>
                <option v-for="environment in workspace.environments" :key="environment.id" :value="environment.id">{{ environment.name }}</option>
              </select>
            </label>
            <button type="button" class="ghost-toggle" @click="envOpen = true"><Icon name="settings" :size="14" />{{ t('http.workspace.manage') }}</button>
            <label class="workspace-field">
              <span>{{ t('http.workspace.collection') }}</span>
              <select v-model="selectedCollectionId" class="select select--compact">
                <option value="">{{ t('http.workspace.none') }}</option>
                <option v-for="collection in workspace.collections" :key="collection.id" :value="collection.id">{{ collection.name }}</option>
              </select>
            </label>
            <button type="button" class="ghost-toggle" @click="saveToCollection"><Icon name="plus" :size="14" />{{ t('http.workspace.saveRequest') }}</button>
            <button type="button" class="ghost-toggle" @click="importOpen = true"><Icon name="upload" :size="14" />{{ t('http.import.action') }}</button>
          </div>
          <div v-if="selectedCollection()?.items.length" class="collection-items">
            <button v-for="item in selectedCollection()?.items" :key="item.id" type="button" class="collection-chip" @click="loadCollectionItem(item)">
              <span class="mono">{{ item.method }}</span>{{ item.name }}
            </button>
          </div>
          <div class="request-line">
            <div class="method-select" :class="`method-select--${method.toLowerCase()}`">
              <select v-model="method" :aria-label="t('http.methodAria')">
                <option v-for="item in methods" :key="item" :value="item">{{ item }}</option>
              </select>
              <Icon name="chevron-down" :size="14" />
            </div>
            <input
              v-model="url"
              class="url-input"
              spellcheck="false"
              placeholder="https://api.example.com/v1/resource"
              @keydown.enter.exact.prevent="send"
            />
            <button class="button button--primary send-button" :disabled="sending" @click="send">
              <span v-if="sending" class="spinner" />
              <Icon v-else name="send" :size="16" />
              {{ sending ? t('http.sending') : t('http.send') }}
            </button>
          </div>

          <div class="request-toolbar">
            <div class="protocol-switch" role="group" :aria-label="t('http.protocol.group')">
              <button :class="{ active: protocol === 'http1' }" @click="protocol = 'http1'">HTTP/1.1</button>
              <button
                :class="{ active: protocol === 'http2' }"
                :disabled="!capabilities.http2"
                :title="capabilities.http2 ? t('http.protocol.http2Title') : t('http.protocol.http2Unavailable')"
                @click="protocol = 'http2'"
              >H2</button>
              <button
                :class="{ active: protocol === 'http3' }"
                :disabled="!capabilities.http3"
                :title="capabilities.http3 ? t('http.protocol.http3Title') : t('http.protocol.http3Unavailable')"
                @click="protocol = 'http3'"
              >H3 <em>{{ t('http.protocol.experimental') }}</em></button>
            </div>
            <div class="request-toolbar__right">
              <button class="ghost-toggle" :class="{ active: advancedOpen }" @click="advancedOpen = !advancedOpen">
                <Icon name="settings" :size="15" /> {{ t('http.options') }}
              </button>
            </div>
          </div>

          <Transition name="reveal">
            <div v-if="advancedOpen" class="advanced-row">
              <label><span>{{ t('http.timeout') }}</span><input v-model.number="timeoutMs" type="number" min="1000" max="120000" /></label>
              <label class="switch-field"><span>{{ t('http.allowInsecureTls') }}</span><button class="switch" :class="{ on: insecure }" @click="insecure = !insecure"><i /></button></label>
              <label class="switch-field"><span>{{ t('http.requestCompression') }}</span><button class="switch" :class="{ on: decompress }" @click="decompress = !decompress"><i /></button></label>
            </div>
          </Transition>
          <Socks5ProxyFields v-if="advancedOpen" v-model="proxy" />

          <div class="sub-tabs">
            <button :class="{ active: requestTab === 'headers' }" @click="requestTab = 'headers'">
              {{ t('http.request.tab.headers') }} <span class="count">{{ cleanHeaders().length }}</span>
            </button>
            <button :class="{ active: requestTab === 'body' }" @click="requestTab = 'body'">{{ t('http.request.tab.body') }}</button>
            <button :class="{ active: requestTab === 'scripts' }" @click="requestTab = 'scripts'">{{ t('http.request.tab.scripts') }}</button>
          </div>

          <div v-show="requestTab === 'headers'" class="editor-pane">
            <div class="kv-head"><span>{{ t('http.headers.name') }}</span><span>{{ t('http.headers.value') }}</span><span /></div>
            <div v-for="(header, index) in headers" :key="index" class="kv-row">
              <input v-model="header.name" :placeholder="t('http.headers.namePlaceholder')" spellcheck="false" class="mono" />
              <input v-model="header.value" :placeholder="t('http.headers.valuePlaceholder')" spellcheck="false" class="mono" />
              <button class="icon-button icon-button--sm" :title="t('http.headers.remove')" @click="removeHeader(index)">
                <Icon name="close" :size="14" />
              </button>
            </div>
            <button class="add-row" @click="addHeader"><Icon name="plus" :size="14" />{{ t('http.headers.add') }}</button>
          </div>

          <div v-show="requestTab === 'body'" class="editor-pane">
            <div class="body-mode">
              <button :class="{ active: bodyMode === 'raw' }" @click="bodyMode = 'raw'">{{ t('http.body.mode.raw') }}</button>
              <button :class="{ active: bodyMode === 'urlencoded' }" @click="bodyMode = 'urlencoded'">{{ t('http.body.mode.urlencoded') }}</button>
              <button :class="{ active: bodyMode === 'multipart' }" @click="bodyMode = 'multipart'">{{ t('http.body.mode.multipart') }}</button>
            </div>

            <template v-if="bodyMode === 'raw'">
              <div class="body-toolbar">
                <select v-model="bodyType" class="select">
                  <option value="application/json">JSON</option>
                  <option value="application/xml">XML</option>
                  <option value="text/plain">{{ t('http.body.typeText') }}</option>
                  <option value="application/x-www-form-urlencoded">{{ t('http.body.typeForm') }}</option>
                  <option value="application/octet-stream">{{ t('http.body.typeBinary') }}</option>
                </select>
                <span class="muted mono">{{ t('http.body.chars', { count: body.length }) }}</span>
              </div>
              <textarea v-model="body" class="code-input" spellcheck="false" :placeholder="t('http.body.placeholder')" />
            </template>

            <template v-else-if="bodyMode === 'urlencoded'">
              <div class="kv-head"><span>{{ t('http.body.fieldName') }}</span><span>{{ t('http.body.fieldValue') }}</span><span /></div>
              <div v-for="(field, index) in urlEncodedFields" :key="index" class="kv-row">
                <input v-model="field.name" :placeholder="t('http.body.fieldNamePlaceholder')" spellcheck="false" class="mono" />
                <input v-model="field.value" :placeholder="t('http.body.fieldValuePlaceholder')" spellcheck="false" class="mono" />
                <button class="icon-button icon-button--sm" :title="t('http.body.removeField')" @click="removeUrlEncodedField(index)"><Icon name="close" :size="14" /></button>
              </div>
              <button class="add-row" @click="addUrlEncodedField"><Icon name="plus" :size="14" />{{ t('http.body.addField') }}</button>
              <p class="muted body-hint">{{ t('http.body.urlencodedHint') }}</p>
            </template>

            <template v-else>
              <div class="multipart-row multipart-row--head">
                <span>{{ t('http.body.fieldType') }}</span>
                <span>{{ t('http.body.fieldName') }}</span>
                <span>{{ t('http.body.fieldValue') }}</span>
                <span />
              </div>
              <div v-for="(field, index) in multipartFields" :key="index" class="multipart-row">
                <select v-model="field.kind" class="select select--compact">
                  <option value="text">{{ t('http.body.fieldText') }}</option>
                  <option value="file">{{ t('http.body.fieldFile') }}</option>
                </select>
                <input v-model="field.name" :placeholder="t('http.body.fieldNamePlaceholder')" spellcheck="false" class="mono" />
                <input v-if="field.kind === 'text'" v-model="field.value" :placeholder="t('http.body.fieldValuePlaceholder')" spellcheck="false" class="mono" />
                <input v-else type="file" class="mono" @change="pickMultipartFile(field, $event)" />
                <button class="icon-button icon-button--sm" :title="t('http.body.removeField')" @click="removeMultipartField(index)"><Icon name="close" :size="14" /></button>
              </div>
              <button class="add-row" @click="addMultipartField"><Icon name="plus" :size="14" />{{ t('http.body.addField') }}</button>
              <p class="muted body-hint">{{ t('http.body.multipartHint') }}</p>
            </template>
          </div>

          <div v-show="requestTab === 'scripts'" class="editor-pane">
            <div class="inline-alert inline-alert--warning">
              <Icon name="alert" :size="16" />{{ t('http.scripts.warning') }}
            </div>
            <label class="field">
              <span>{{ t('http.scripts.pre') }}</span>
              <textarea v-model="preScript" class="code-input code-input--short" spellcheck="false" :placeholder="t('http.scripts.prePlaceholder')" />
            </label>
            <label class="field">
              <span>{{ t('http.scripts.post') }}</span>
              <textarea v-model="postScript" class="code-input code-input--short" spellcheck="false" :placeholder="t('http.scripts.postPlaceholder')" />
            </label>
            <p class="muted body-hint">{{ t('http.scripts.hint') }}</p>
            <div v-if="scriptTests.length" class="script-tests">
              <div v-for="(test, index) in scriptTests" :key="index" class="script-test" :class="{ 'script-test--bad': !test.passed }">
                <strong>{{ test.passed ? 'PASS' : 'FAIL' }}</strong>
                <span>{{ test.name }}</span>
                <small v-if="test.error">{{ test.error }}</small>
              </div>
            </div>
          </div>
        </section>

        <section class="response-card">
          <header class="response-head">
            <div class="response-head__title">
              <span class="eyebrow">{{ t('http.response.eyebrow') }}</span>
              <div v-if="result" class="response-meta">
                <span class="status-pill" :class="`status-pill--${statusTone}`">
                  {{ result.ok ? result.status : 'ERR' }}
                </span>
                <span class="mono">{{ result.ok ? result.statusText : result.error }}</span>
              </div>
              <span v-else class="muted">{{ t('http.response.waiting') }}</span>
            </div>
            <div class="response-head__actions">
              <template v-if="result?.ok">
                <span class="meta-pill mono">{{ result.httpVersion }}</span>
                <span class="meta-pill mono">{{ result.timing?.totalMs ?? 0 }} ms</span>
                <span class="meta-pill mono">{{ formatBytes(result.bodySize ?? 0) }}</span>
                <button class="icon-button icon-button--sm" :title="t('http.response.copyBody')" @click="copy(responseBody)"><Icon name="copy" :size="15" /></button>
                <button class="icon-button icon-button--sm" :title="result.bodyTruncated ? t('http.download.streamTitle') : t('http.response.downloadBody')" @click="downloadBody"><Icon name="download" :size="15" /></button>
                <span v-if="downloading || downloadState" class="meta-pill mono">
                  {{ downloadState?.totalBytes && downloadState.totalBytes > 0
                    ? t('http.download.progressTotal', { bytes: formatBytes(downloadState.bytes), total: formatBytes(downloadState.totalBytes) })
                    : t('http.download.progress', { bytes: formatBytes(downloadState?.bytes ?? 0) }) }}
                </span>
                <button v-if="downloading" class="icon-button icon-button--sm" :title="t('http.download.cancel')" @click="cancelDownload"><Icon name="close" :size="14" /></button>
              </template>
            </div>
          </header>

          <div v-if="sending" class="response-loading">
            <div class="pulse-orbit"><span /><span /><span /></div>
            <div>
              <strong>{{ t('http.response.waitingOn', { protocol: protocol.toUpperCase() }) }}</strong>
              <p class="mono">{{ t('http.response.elapsed', { elapsed }) }}</p>
            </div>
          </div>

          <div v-else-if="!result" class="response-empty">
            <div class="response-empty__art"><Icon name="send" :size="30" /></div>
            <h3>{{ t('http.response.readyTitle') }}</h3>
            <p>{{ t('http.response.readyLead') }}</p>
          </div>

          <div v-else-if="!result.ok" class="response-error">
            <div class="inline-alert inline-alert--danger">
              <Icon name="alert" :size="17" />
              <span>{{ result.error || t('http.requestFailed') }}</span>
            </div>
            <div class="error-details mono">
              {{ result.request?.method }} {{ result.request?.url }} · {{ result.timing?.totalMs ?? 0 }} ms
            </div>
          </div>

          <template v-else>
            <div class="sub-tabs sub-tabs--response">
              <button :class="{ active: responseTab === 'body' }" @click="responseTab = 'body'">
                {{ t('http.response.tab.body') }} <span v-if="responseIsBinary" class="count">base64/hex</span>
              </button>
              <button
                v-if="previewAvailable"
                :class="{ active: responseTab === 'preview' }"
                @click="responseTab = 'preview'"
              >
                {{ t('http.response.tab.preview') }} <span class="count">{{ previewKind }}</span>
              </button>
              <button :class="{ active: responseTab === 'headers' }" @click="responseTab = 'headers'">
                {{ t('http.response.tab.headers') }} <span class="count">{{ result.headers?.length ?? 0 }}</span>
              </button>
              <button :class="{ active: responseTab === 'timing' }" @click="responseTab = 'timing'">{{ t('http.response.tab.timing') }}</button>
            </div>
            <div v-if="responseTab === 'body'" class="response-body">
              <pre class="code-output">{{ responseBody }}</pre>
              <div v-if="result.bodyTruncated" class="truncation-note">
                <Icon name="alert" :size="14" />{{ t('http.response.truncated') }}
              </div>
            </div>
            <div v-else-if="responseTab === 'preview'" class="response-preview">
              <div class="preview-toolbar">
                <span class="chip mono">{{ contentType || t('http.response.unknownContentType') }}</span>
                <span class="muted">{{ formatBytes(result.bodySize ?? 0) }}</span>
                <label v-if="previewKind === 'html'" class="preview-toggle">
                  <input v-model="allowRemotePreview" type="checkbox" :disabled="allowScriptsPreview" />
                  {{ t('http.preview.loadRemote') }}
                </label>
                <label v-if="previewKind === 'html'" class="preview-toggle preview-toggle--unsafe">
                  <input type="checkbox" :checked="allowScriptsPreview" @change="toggleScripts" />
                  {{ t('http.preview.runScripts') }}
                </label>
                <span class="muted preview-hint">
                  <template v-if="previewKind !== 'html'">{{ t('http.preview.localBrowser') }}</template>
                  <template v-else-if="allowScriptsPreview">{{ t('http.preview.scriptsEnabled') }}</template>
                  <template v-else>{{ t('http.preview.scriptsDisabled') }}</template>
                </span>
              </div>
              <div v-if="previewKind === 'html' && !allowRemotePreview && !allowScriptsPreview" class="preview-warning">
                <Icon name="alert" :size="14" />
                {{ t('http.preview.remoteHint') }}
              </div>
              <div v-if="allowScriptsPreview" class="preview-warning">
                <Icon name="alert" :size="14" />
                {{ t('http.preview.scriptsHint') }}
              </div>
              <div v-if="result.bodyTruncated" class="preview-warning">
                <Icon name="alert" :size="14" />
                {{ t('http.preview.warning') }}
              </div>
              <iframe
                v-if="previewKind === 'html' && allowScriptsPreview && previewDocUrl"
                class="preview-frame preview-frame--html"
                sandbox="allow-scripts"
                :src="previewDocUrl"
                referrerpolicy="no-referrer"
                :title="t('http.preview.htmlTitle')"
              />
              <div v-else-if="previewKind === 'html' && allowScriptsPreview" class="preview-empty">
                {{ t('http.preview.scriptsPreparing') }}
              </div>
              <iframe
                v-else-if="previewKind === 'html' && allowRemotePreview && previewDocUrl"
                class="preview-frame preview-frame--html"
                sandbox=""
                :src="previewDocUrl"
                referrerpolicy="no-referrer"
                :title="t('http.preview.htmlTitle')"
              />
              <div v-else-if="previewKind === 'html' && allowRemotePreview" class="preview-empty">
                {{ t('http.preview.scriptsPreparing') }}
              </div>
              <iframe
                v-else-if="previewKind === 'html'"
                class="preview-frame preview-frame--html"
                sandbox=""
                :srcdoc="htmlPreviewDocument"
                :title="t('http.preview.htmlTitle')"
              />
              <div v-else-if="previewKind === 'image'" class="preview-media">
                <img :src="previewUrl" :alt="t('http.preview.imageAlt')" />
              </div>
              <audio v-else-if="previewKind === 'audio'" class="preview-audio" :src="previewUrl" controls />
              <video v-else-if="previewKind === 'video'" class="preview-video" :src="previewUrl" controls />
              <iframe
                v-else-if="previewKind === 'pdf'"
                class="preview-frame"
                :src="previewUrl"
                :title="t('http.preview.pdfTitle')"
              />
              <div v-else class="preview-empty">{{ t('http.preview.empty') }}</div>
            </div>
            <div v-else-if="responseTab === 'headers'" class="response-headers">
              <div v-for="header in result.headers" :key="header.name + header.value" class="header-line">
                <span class="header-name mono">{{ header.name }}</span>
                <span class="header-value mono">{{ header.value }}</span>
              </div>
            </div>
            <div v-else class="timing-panel">
              <div class="timing-row">
                <div class="timing-row__label"><Icon name="globe" :size="15" />{{ t('http.timing.connect') }}</div>
                <div class="timing-bar"><span :style="{ width: `${Math.min(100, ((result.timing?.connectMs ?? 0) / Math.max(1, result.timing?.totalMs ?? 1)) * 100)}%` }" /></div>
                <span class="mono">{{ result.timing?.connectMs ?? 0 }} ms</span>
              </div>
              <div class="timing-row">
                <div class="timing-row__label"><Icon name="bolt" :size="15" />{{ t('http.timing.total') }}</div>
                <div class="timing-bar timing-bar--total"><span style="width: 100%" /></div>
                <span class="mono">{{ result.timing?.totalMs ?? 0 }} ms</span>
              </div>
              <div class="timing-facts">
                <div><span>{{ t('http.timing.protocol') }}</span><strong>{{ result.httpVersion || '—' }}</strong></div>
                <div><span>ALPN</span><strong class="mono">{{ result.alpn || '—' }}</strong></div>
                <div><span>{{ t('http.timing.bodyBytes') }}</span><strong>{{ result.bodySize ?? 0 }}</strong></div>
                <div v-if="result.compression?.contentEncoding">
                  <span>{{ t('http.compression.encoding') }}</span>
                  <strong class="mono">{{ result.compression.contentEncoding }}{{ result.compression.decoded ? ' ✓' : '' }}</strong>
                </div>
                <div v-if="result.compression?.wireBodySize !== undefined">
                  <span>{{ t('http.compression.wire') }}</span>
                  <strong class="mono">
                    {{ formatBytes(result.compression.wireBodySize) }} → {{ formatBytes(result.compression.bodySize) }}
                  </strong>
                </div>
                <div v-if="result.compression?.error">
                  <span>{{ t('http.compression.failed') }}</span>
                  <strong class="mono">{{ result.compression.error }}</strong>
                </div>
                <div><span>{{ t('http.timing.streamId') }}</span><strong class="mono">{{ result.streamId ?? 0 }}</strong></div>
                <div><span>{{ t('http.timing.retryable') }}</span><strong>{{ result.retryable ? t('http.timing.yes') : t('http.timing.no') }}</strong></div>
                <div v-if="result.goaway">
                  <span>{{ t('http.timing.goaway') }}</span>
                  <strong class="mono">
                    {{ result.goaway.received ? `${result.goaway.errorCode} / ${result.goaway.lastStreamId}` : t('http.timing.no') }}
                  </strong>
                </div>
                <div><span>{{ t('http.timing.sentAt') }}</span><strong class="mono">{{ result.startedAt }}</strong></div>
              </div>
              <div v-if="result.h2" class="h2-frames">
                <header class="h2-frames__head">
                  <span>{{ t('http.h2.title') }}</span>
                  <span class="mono">{{ t('http.h2.count', { count: result.h2.frameCount }) }}</span>
                </header>
                <div v-if="result.h2.frames.length" class="h2-frames__list">
                  <div v-for="(frame, index) in result.h2.frames" :key="index" class="h2-frame">
                    <span class="h2-frame__type">{{ frame.type }}</span>
                    <span class="mono" :title="t('http.h2.stream')">{{ frame.streamId }}</span>
                    <span class="mono" :title="t('http.h2.length')">{{ frame.length }} B</span>
                    <span class="mono" :title="t('http.h2.flags')">
                      {{ frame.flags ? `0x${frame.flags.toString(16)}` : '—' }}
                    </span>
                    <span v-if="frame.errorCode" class="h2-frame__error mono">{{ frame.errorCode }}</span>
                  </div>
                </div>
                <p v-else class="h2-frames__empty">{{ t('http.h2.empty') }}</p>
                <p v-if="result.h2.framesTruncated" class="h2-frames__hint">
                  {{ t('http.h2.truncated', { limit: result.h2.maxFrames }) }}
                </p>
              </div>
            </div>
          </template>
        </section>
      </div>

      <aside class="history-panel">
        <header class="history-head">
          <div><span class="eyebrow">{{ t('http.history.eyebrow') }}</span><h3>{{ t('http.history.title') }}</h3></div>
          <button class="icon-button icon-button--sm" :title="t('http.history.refresh')" @click="loadHistory"><Icon name="refresh" :size="15" /></button>
        </header>
        <div v-if="historyLoading" class="history-empty"><span class="spinner" />{{ t('http.history.loading') }}</div>
        <div v-else-if="history.length" class="history-list">
          <button v-for="entry in history" :key="entry.id" class="history-item" @click="restore(entry)">
            <span class="status-code" :class="entry.status === 'success' ? 'status-code--ok' : 'status-code--bad'">{{ entry.httpStatus || 'ERR' }}</span>
            <div class="history-item__body">
              <strong class="mono">{{ entry.summary }}</strong>
              <span>{{ formatTime(entry.createdAt) }} · {{ entry.latencyMs }} ms</span>
            </div>
          </button>
        </div>
        <div v-else class="history-empty">
          <Icon name="clock" :size="18" />
          <span>{{ t('http.history.empty') }}</span>
        </div>
        <button v-if="history.length && auth.isAdmin" class="history-clear" @click="clearHistory">
          <Icon name="trash" :size="14" />{{ t('http.history.clear') }}
        </button>
      </aside>
    </div>

    <Teleport to="body">
      <Transition name="modal">
        <div v-if="envOpen" class="modal-scrim" @click.self="envOpen = false">
          <div class="modal-card modal-card--wide">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('http.workspace.environment') }}</span><h3>{{ t('http.workspace.title') }}</h3></div>
              <button type="button" class="icon-button" @click="envOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <div class="workspace-envs">
                <div v-for="environment in workspace.environments" :key="environment.id" class="workspace-env-row">
                  <button type="button" class="link-button" @click="editEnvironment(environment)">{{ environment.name }}</button>
                  <button type="button" class="icon-button icon-button--sm icon-button--danger" @click="deleteEnvironment(environment.id)"><Icon name="trash" :size="14" /></button>
                </div>
                <button type="button" class="link-button" @click="openEnvironment"><Icon name="plus" :size="14" />{{ t('http.workspace.newEnvironment') }}</button>
              </div>
              <div class="form-grid">
                <label class="field field--grow"><span>{{ t('http.workspace.name') }}</span><input v-model="envDraft.name" :placeholder="t('http.workspace.namePlaceholder')" /></label>
              </div>
              <div class="kv-head"><span>{{ t('http.workspace.variable') }}</span><span>{{ t('http.workspace.value') }}</span><span /></div>
              <div v-for="(variable, index) in envDraft.variables" :key="index" class="kv-row">
                <input v-model="variable.key" class="mono" placeholder="base_url" spellcheck="false" />
                <input v-model="variable.value" class="mono" placeholder="https://api.example.com" spellcheck="false" />
                <button type="button" class="icon-button icon-button--sm" @click="removeWorkspaceVariable(index)"><Icon name="close" :size="14" /></button>
              </div>
              <button type="button" class="add-row" @click="addWorkspaceVariable"><Icon name="plus" :size="14" />{{ t('http.workspace.addVariable') }}</button>
              <p class="muted body-hint">{{ t('http.workspace.variableHint') }}</p>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="envOpen = false">{{ t('http.curl.close') }}</button>
              <button type="button" class="button button--primary" @click="saveEnvironment"><Icon name="check" :size="15" />{{ t('http.workspace.saveEnvironment') }}</button>
            </footer>
          </div>
        </div>
      </Transition>
      <Transition name="modal">
        <div v-if="importOpen" class="modal-scrim" @click.self="importOpen = false">
          <div class="modal-card modal-card--wide">
            <header class="modal-head">
              <div><span class="eyebrow">{{ t('http.import.eyebrow') }}</span><h3>{{ t('http.import.title') }}</h3></div>
              <button type="button" class="icon-button" @click="importOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <p class="modal-hint">{{ t('http.import.hint') }}</p>
              <textarea v-model="importText" class="code-input" spellcheck="false" :placeholder="t('http.import.placeholder')" />
              <label class="link-button">{{ t('http.import.file') }}<input type="file" accept=".json,application/json" class="hidden-file" @change="importCollectionFile" /></label>
              <div v-if="importError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ importError }}</div>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="importOpen = false">{{ t('http.curl.cancel') }}</button>
              <button type="button" class="button button--primary" :disabled="!importText.trim()" @click="applyImport"><Icon name="upload" :size="15" />{{ t('http.import.action') }}</button>
            </footer>
          </div>
        </div>
      </Transition>
      <Transition name="modal">
        <div v-if="curlOpen" class="modal-scrim" @click.self="curlOpen = false">
          <div class="modal-card modal-card--wide">
            <header class="modal-head">
              <div><span class="eyebrow">cURL</span><h3>{{ t('http.curl.exportTitle') }}</h3></div>
              <button type="button" class="icon-button" @click="curlOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <textarea v-model="curlText" class="code-input" readonly spellcheck="false" />
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="curlOpen = false">{{ t('http.curl.close') }}</button>
              <button type="button" class="button button--primary" @click="copyCurl"><Icon name="copy" :size="15" />{{ t('http.curl.copy') }}</button>
            </footer>
          </div>
        </div>
      </Transition>
      <Transition name="modal">
        <div v-if="curlImportOpen" class="modal-scrim" @click.self="curlImportOpen = false">
          <div class="modal-card modal-card--wide">
            <header class="modal-head">
              <div><span class="eyebrow">cURL</span><h3>{{ t('http.curl.importTitle') }}</h3></div>
              <button type="button" class="icon-button" @click="curlImportOpen = false"><Icon name="close" :size="18" /></button>
            </header>
            <div class="modal-body">
              <p class="modal-hint">{{ t('http.curl.importHint') }}</p>
              <textarea v-model="curlImportText" class="code-input" spellcheck="false" placeholder="curl -X POST 'https://…' -H 'content-type: application/json' -d '{…}'" />
              <div v-if="curlImportError" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ curlImportError }}</div>
            </div>
            <footer class="modal-foot">
              <button type="button" class="button" @click="curlImportOpen = false">{{ t('http.curl.cancel') }}</button>
              <button type="button" class="button button--primary" @click="applyCurlImport"><Icon name="download" :size="15" />{{ t('http.curl.importAction') }}</button>
            </footer>
          </div>
        </div>
      </Transition>
    </Teleport>
  </div>
</template>
