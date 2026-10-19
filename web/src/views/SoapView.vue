<script setup lang="ts">
import { computed, ref } from 'vue'
import Icon from '../components/Icon.vue'
import { errorMessage, post } from '../api'
import { useUiStore } from '../stores/ui'
import { useI18nStore } from '../stores/i18n'
import type { HttpResult } from '../types'

defineOptions({ name: 'SoapView' })

const ui = useUiStore()
const i18n = useI18nStore()
const t = i18n.t
const wsdlUrl = ref('')
const wsdlText = ref('')
const loadingWsdl = ref(false)
const operations = ref<Array<{ name: string; action: string; documentation: string }>>([])
const targetNamespace = ref('')
const endpoint = ref('')
const selected = ref('')
const envelope = ref('')
const sending = ref(false)
const response = ref<HttpResult | null>(null)
const responseBody = ref('')
const fault = ref<{ code: string; message: string } | null>(null)
const search = ref('')
const showWsdl = ref(false)
const requestTab = ref<'envelope' | 'headers'>('envelope')

const filteredOperations = computed(() =>
  operations.value.filter((operation) => operation.name.toLowerCase().includes(search.value.toLowerCase())),
)
const selectedOperation = computed(() => operations.value.find((operation) => operation.name === selected.value))

function decodeResponseBody(result: HttpResult): string {
  if (result.bodyEncoding === 'base64' && result.bodyBase64) {
    try {
      return atob(result.bodyBase64)
    } catch {
      return ''
    }
  }
  return result.bodyText || ''
}

function parseWsdl(xml: string) {
  operations.value = []
  targetNamespace.value = ''
  endpoint.value = ''
  fault.value = null
  response.value = null
  let doc: Document
  try {
    doc = new DOMParser().parseFromString(xml, 'application/xml')
  } catch {
    ui.notify('error', t('soap.msg.wsdlParseError'))
    return
  }
  if (doc.querySelector('parsererror')) {
    ui.notify('error', t('soap.msg.wsdlMalformed'))
    return
  }
  const definitions = doc.documentElement
  if (!definitions || !definitions.localName.toLowerCase().includes('definitions')) {
    ui.notify('warning', t('soap.msg.notWsdl'))
    return
  }
  targetNamespace.value = definitions.getAttribute('targetNamespace') || ''
  const bindingActions = new Map<string, string>()
  const bindings = Array.from(doc.getElementsByTagNameNS('*', 'binding'))
  for (const binding of bindings) {
    for (const operation of Array.from(binding.getElementsByTagNameNS('*', 'operation'))) {
      const name = operation.getAttribute('name') || ''
      const soapOperation = Array.from(operation.getElementsByTagNameNS('*', 'operation'))[0]
      const action = soapOperation?.getAttribute('soapAction') || operation.getAttribute('soapAction') || ''
      if (name && action) bindingActions.set(name, action)
    }
  }
  for (const port of Array.from(doc.getElementsByTagNameNS('*', 'portType'))) {
    for (const operation of Array.from(port.getElementsByTagNameNS('*', 'operation'))) {
      const name = operation.getAttribute('name') || ''
      if (!name) continue
      const docs = Array.from(operation.getElementsByTagNameNS('*', 'documentation')).map((node) => node.textContent || '').join(' ').trim()
      operations.value.push({ name, action: bindingActions.get(name) || '', documentation: docs })
    }
  }
  const addresses = Array.from(doc.getElementsByTagNameNS('*', 'address'))
  const soapAddress = addresses.find((node) => node.getAttribute('location'))
  if (soapAddress) endpoint.value = soapAddress.getAttribute('location') || ''
  if (operations.value.length) {
    selected.value = operations.value[0].name
    buildEnvelope()
    ui.notify('success', t('soap.msg.wsdlParsed'), t('soap.msg.wsdlParsedBody', { count: operations.value.length }))
  } else {
    ui.notify('warning', t('soap.msg.noOperations'))
  }
}

async function fetchWsdl() {
  if (!wsdlUrl.value.trim()) {
    ui.notify('warning', t('soap.msg.enterUrl'))
    return
  }
  loadingWsdl.value = true
  try {
    const result = await post<HttpResult>('/api/http/send', {
      url: wsdlUrl.value.trim(),
      method: 'GET',
      protocol: 'http1',
      headers: [{ name: 'accept', value: 'text/xml, application/wsdl+xml, application/xml' }],
      body: '',
      compression: false,
      timeoutMs: 20000,
    })
    if (!result.ok) {
      ui.notify('error', t('soap.msg.fetchFailed'), result.error)
      return
    }
    wsdlText.value = decodeResponseBody(result)
    parseWsdl(wsdlText.value)
    showWsdl.value = true
  } catch (err) {
    ui.toastError(err, t('soap.msg.fetchFailed'))
  } finally {
    loadingWsdl.value = false
  }
}

function buildEnvelope() {
  const operation = selectedOperation.value
  if (!operation) return
  const ns = targetNamespace.value || 'urn:example'
  envelope.value =
    `<soapenv:Envelope xmlns:soapenv="http://schemas.xmlsoap.org/soap/envelope/" xmlns:ns="${ns}">\n` +
    `  <soapenv:Header/>\n` +
    `  <soapenv:Body>\n` +
    `    <ns:${operation.name}>\n` +
    `      <!-- ${t('soap.envelope.comment')} -->\n` +
    `    </ns:${operation.name}>\n` +
    `  </soapenv:Body>\n` +
    `</soapenv:Envelope>`
  requestTab.value = 'envelope'
}

function selectOperation(name: string) {
  selected.value = name
  buildEnvelope()
  response.value = null
  responseBody.value = ''
  fault.value = null
}

function parseFault(xml: string) {
  try {
    const doc = new DOMParser().parseFromString(xml, 'application/xml')
    const faultNode = doc.getElementsByTagNameNS('*', 'Fault')[0]
    if (!faultNode) return null
    const code = faultNode.getElementsByTagNameNS('*', 'faultcode')[0]?.textContent || ''
    const message =
      faultNode.getElementsByTagNameNS('*', 'faultstring')[0]?.textContent ||
      faultNode.getElementsByTagNameNS('*', 'Text')[0]?.textContent ||
      ''
    return { code, message }
  } catch {
    return null
  }
}

async function sendSoap() {
  if (!endpoint.value.trim()) {
    ui.notify('warning', t('soap.msg.endpointRequired'))
    return
  }
  if (!envelope.value.trim()) {
    ui.notify('warning', t('soap.msg.envelopeEmpty'))
    return
  }
  sending.value = true
  response.value = null
  fault.value = null
  try {
    const headers = [
      { name: 'content-type', value: 'text/xml; charset=utf-8' },
      { name: 'accept', value: 'text/xml, application/soap+xml' },
    ]
    if (selectedOperation.value?.action) headers.push({ name: 'SOAPAction', value: selectedOperation.value.action })
    const result = await post<HttpResult>('/api/http/send', {
      url: endpoint.value.trim(),
      method: 'POST',
      protocol: 'http1',
      headers,
      body: envelope.value,
      compression: false,
      timeoutMs: 30000,
    })
    response.value = result
    responseBody.value = decodeResponseBody(result)
    fault.value = parseFault(responseBody.value)
    if (!result.ok) ui.notify('error', t('soap.msg.callFailed'), result.error)
    else if (fault.value) ui.notify('error', t('soap.msg.fault'), fault.value.message)
    else ui.notify('success', t('soap.msg.responseReceived'))
  } catch (err) {
    ui.toastError(err, t('soap.msg.callFailed'))
  } finally {
    sending.value = false
  }
}

function prettyResponse() {
  if (!responseBody.value) return ''
  try {
    const doc = new DOMParser().parseFromString(responseBody.value, 'application/xml')
    if (doc.querySelector('parsererror')) return responseBody.value
    return new XMLSerializer().serializeToString(doc).replace(/></g, '>\n<')
  } catch {
    return responseBody.value
  }
}
</script>

<template>
  <div class="page soap-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">SOAP / WSDL</span>
        <h2>{{ t('soap.head.title') }}</h2>
        <p>{{ t('soap.head.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <span class="chip mono">{{ t('soap.head.operations', { count: operations.length }) }}</span>
      </div>
    </header>

    <section class="card wsdl-bar">
      <div class="wsdl-bar__line">
        <span class="protocol-badge protocol-badge--soap">WSDL</span>
        <input v-model="wsdlUrl" class="url-input" placeholder="https://service.example.com?wsdl" spellcheck="false" @keydown.enter.prevent="fetchWsdl" />
        <button class="button button--primary" :disabled="loadingWsdl" @click="fetchWsdl">
          <span v-if="loadingWsdl" class="spinner" /><Icon v-else name="download" :size="16" />{{ t('soap.fetch.label') }}
        </button>
        <button class="button" @click="showWsdl = !showWsdl"><Icon name="braces" :size="16" />{{ t('soap.fetch.pasteXml') }}</button>
      </div>
      <Transition name="reveal">
        <div v-if="showWsdl" class="wsdl-source">
          <textarea v-model="wsdlText" class="code-input" spellcheck="false" :placeholder="t('soap.fetch.placeholder')" />
          <button class="button" @click="parseWsdl(wsdlText)"><Icon name="braces" :size="15" />{{ t('soap.fetch.parsePasted') }}</button>
        </div>
      </Transition>
    </section>

    <div class="soap-grid">
      <aside class="card soap-ops">
        <header class="card__head"><div><span class="eyebrow">{{ t('soap.contract.eyebrow') }}</span><h3>{{ t('soap.operations.title') }}</h3></div></header>
        <div class="search-box">
          <Icon name="search" :size="15" />
          <input v-model="search" :placeholder="t('soap.operations.filter')" spellcheck="false" />
        </div>
        <div v-if="filteredOperations.length" class="op-list">
          <button v-for="operation in filteredOperations" :key="operation.name" class="op-item" :class="{ 'op-item--active': selected === operation.name }" @click="selectOperation(operation.name)">
            <span class="op-item__icon"><Icon name="braces" :size="15" /></span>
            <span class="op-item__copy"><strong>{{ operation.name }}</strong><small>{{ operation.action || t('soap.operations.noAction') }}</small></span>
          </button>
        </div>
        <div v-else class="history-empty"><Icon name="search" :size="18" /><span>{{ t('soap.operations.empty') }}</span></div>
        <div v-if="targetNamespace" class="ns-box">
          <span class="eyebrow">{{ t('soap.operations.targetNamespace') }}</span>
          <code class="mono">{{ targetNamespace }}</code>
        </div>
      </aside>

      <section class="soap-main">
        <article class="card soap-request">
          <header class="card__head">
            <div><span class="eyebrow">{{ t('soap.request.eyebrow') }}</span><h3>{{ selected || t('soap.request.selectOperation') }}</h3></div>
            <span v-if="selectedOperation?.action" class="chip mono">{{ selectedOperation.action }}</span>
          </header>
          <label class="field">
            <span>{{ t('soap.request.endpoint') }}</span>
            <input v-model="endpoint" class="mono" spellcheck="false" placeholder="https://service.example.com/Service" />
          </label>
          <div class="sub-tabs">
            <button :class="{ active: requestTab === 'envelope' }" @click="requestTab = 'envelope'">{{ t('soap.request.tabEnvelope') }}</button>
            <button :class="{ active: requestTab === 'headers' }" @click="requestTab = 'headers'">{{ t('soap.request.tabHeaders') }}</button>
          </div>
          <textarea v-show="requestTab === 'envelope'" v-model="envelope" class="code-input code-input--tall" spellcheck="false" />
          <div v-show="requestTab === 'headers'" class="soap-headers">
            <div class="header-line"><span class="header-name mono">content-type</span><span class="header-value mono">text/xml; charset=utf-8</span></div>
            <div class="header-line"><span class="header-name mono">SOAPAction</span><span class="header-value mono">{{ selectedOperation?.action || t('soap.request.none') }}</span></div>
            <div class="header-line"><span class="header-name mono">accept</span><span class="header-value mono">text/xml, application/soap+xml</span></div>
          </div>
          <div class="form-actions">
            <span class="muted">{{ t('soap.request.meta', { count: envelope.length }) }}</span>
            <button class="button button--primary" :disabled="sending || !selected" @click="sendSoap">
              <span v-if="sending" class="spinner" /><Icon v-else name="send" :size="16" />{{ t('soap.request.invoke') }}
            </button>
          </div>
        </article>

        <article class="card soap-response">
          <header class="card__head">
            <div><span class="eyebrow">{{ t('soap.response.eyebrow') }}</span><h3>{{ t('soap.response.title') }}</h3></div>
            <div v-if="response" class="response-meta">
              <span class="status-pill" :class="response.ok && !fault ? 'status-pill--ok' : 'status-pill--bad'">{{ response.ok ? response.status : t('soap.response.errorStatus') }}</span>
              <span class="meta-pill mono">{{ response.timing?.totalMs ?? 0 }} ms</span>
            </div>
          </header>
          <div v-if="!response" class="response-empty response-empty--compact">
            <Icon name="braces" :size="24" />
            <h3>{{ t('soap.response.empty.title') }}</h3>
            <p>{{ t('soap.response.empty.body') }}</p>
          </div>
          <template v-else>
            <div v-if="fault" class="fault-card">
              <div class="fault-card__head"><Icon name="alert" :size="16" />{{ t('soap.msg.fault') }}</div>
              <strong>{{ fault.code || 'Fault' }}</strong>
              <p>{{ fault.message || t('soap.response.faultNoDescription') }}</p>
            </div>
            <div v-if="!response.ok" class="inline-alert inline-alert--danger"><Icon name="alert" :size="16" />{{ response.error }}</div>
            <pre class="code-output code-output--soap">{{ prettyResponse() || t('soap.response.emptyBody') }}</pre>
            <details class="raw-details">
              <summary>{{ t('soap.response.rawHeaders') }}</summary>
              <div v-for="header in response.headers" :key="header.name + header.value" class="header-line">
                <span class="header-name mono">{{ header.name }}</span><span class="header-value mono">{{ header.value }}</span>
              </div>
            </details>
          </template>
        </article>
      </section>
    </div>
  </div>
</template>
