<script setup lang="ts">
import Icon from '../components/Icon.vue'
import { useAuthStore } from '../stores/auth'
import { useRouter } from 'vue-router'

defineOptions({ name: 'HelpView' })
const auth = useAuthStore()
const router = useRouter()

const modules = [
  { icon: 'bolt', title: 'HTTP Studio', text: 'Send any method, tune headers and body, and read status, timing and raw bytes. Ctrl+Enter sends from anywhere in the request pane.' },
  { icon: 'pulse', title: 'WebSocket console', text: 'Connects from the browser, so the URL must be reachable by this machine. Text and hex frames are captured in the tab while it is cached.' },
  { icon: 'layers', title: 'TCP / UDP bench', text: 'One-shot probes through libuvcpp. Hex mode accepts spaced, colon-separated or plain hex payloads.' },
  { icon: 'braces', title: 'SOAP / WSDL', text: 'Fetch a WSDL through the daemon, pick an operation, edit the generated SOAP 1.1 envelope, then invoke it like any HTTP request.' },
  { icon: 'shield', title: 'TLS Inspector', text: 'Decodes the first PEM or DER certificate in the input. Use the fingerprint, SAN and validity data to verify what a service presents.' },
  { icon: 'route', title: 'Reverse proxy', text: 'Each listener owns a host:port and forwards HTTP/1.1 requests to one upstream. Logs stream into the gateway panel every few seconds.' },
]

const shortcuts = [
  ['Ctrl / ⌘ + Enter', 'Send the current HTTP request or WebSocket message'],
  ['Right-click a tab', 'Close every other closable tab'],
  ['Click a history row', 'Restore method, URL and protocol preference'],
  ['Ctrl / ⌘ + K', 'Focus the browser address bar (browser shortcut)'],
]
</script>

<template>
  <div class="page help-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">Help</span>
        <h2>Start with one handshake.</h2>
        <p>Short guides for every workbench, plus the operational details that make local debugging predictable.</p>
      </div>
      <div class="page-head__actions">
        <button class="button" @click="router.push('/http')"><Icon name="bolt" :size="16" />Open HTTP Studio</button>
      </div>
    </header>

    <section class="help-hero">
      <div class="help-hero__copy">
        <span class="eyebrow">Quick start</span>
        <h3>Three moves to a useful trace.</h3>
        <ol class="quick-steps">
          <li><span>1</span><div><strong>Pick a module</strong><p>Use the left rail. Each module opens a cached tab and keeps its state.</p></div></li>
          <li><span>2</span><div><strong>Send something small</strong><p>A GET, a WebSocket ping or a TCP HEAD request is enough to validate connectivity.</p></div></li>
          <li><span>3</span><div><strong>Read the wire</strong><p>Headers, timing and byte counts are all local. Use history to replay a shape.</p></div></li>
        </ol>
      </div>
      <div class="help-hero__terminal">
        <div class="terminal-bar"><i /><i /><i /><span class="mono">uvscope --port 8787</span></div>
        <pre><span class="t-dim">$</span> uvscope --port 8787
<span class="t-accent">uvscope 0.1.0</span>
  workbench : http://127.0.0.1:8787
  data      : {{ auth.dataDir }}
  libuvcpp  : {{ auth.libuvcpp || '1.8.0' }}
  engine    : libuv</pre>
        <div class="terminal-foot"><span class="status-dot" />loop running · Ctrl+C to stop</div>
      </div>
    </section>

    <section class="module-help-grid">
      <article v-for="module in modules" :key="module.title" class="card module-help">
        <span class="module-help__icon"><Icon :name="module.icon" :size="19" /></span>
        <h3>{{ module.title }}</h3>
        <p>{{ module.text }}</p>
      </article>
    </section>

    <div class="help-grid">
      <section class="card">
        <header class="card__head"><div><span class="eyebrow">Keyboard</span><h3>Shortcuts</h3></div><Icon name="terminal" :size="18" class="muted" /></header>
        <div class="shortcut-list">
          <div v-for="row in shortcuts" :key="row[0]" class="shortcut-row"><kbd>{{ row[0] }}</kbd><span>{{ row[1] }}</span></div>
        </div>
      </section>

      <section class="card">
        <header class="card__head"><div><span class="eyebrow">Operations</span><h3>Local behaviour</h3></div><Icon name="database" :size="18" class="muted" /></header>
        <div class="note-list">
          <div class="note-row"><span>Storage</span><strong>SQLite in the data directory. Delete it while stopped to reset.</strong></div>
          <div class="note-row"><span>Sessions</span><strong>HttpOnly cookie, seven-day expiry, server-side lookup.</strong></div>
          <div class="note-row"><span>Network</span><strong>All requests originate from the uvscope daemon, not the browser.</strong></div>
          <div class="note-row"><span>Exposure</span><strong>Bind to 127.0.0.1 unless you have a trusted private network.</strong></div>
        </div>
      </section>

      <section class="card">
        <header class="card__head"><div><span class="eyebrow">Troubleshooting</span><h3>When something looks stuck</h3></div><Icon name="help" :size="18" class="muted" /></header>
        <details class="help-detail" open>
          <summary>HTTPS fails to connect</summary>
          <p>Use a CA bundle that matches the daemon and verify the system clock. For private lab services, enable <em>Allow insecure TLS</em> only while testing.</p>
        </details>
        <details class="help-detail">
          <summary>HTTP/2 or HTTP/3 errors</summary>
          <p>Protocol negotiation is experimental in 0.1.0. Use HTTP/1.1 for production debugging; the daemon reports the negotiated ALPN in the timing tab.</p>
        </details>
        <details class="help-detail">
          <summary>A proxy port will not bind</summary>
          <p>Another process owns the port, or the listener is already running. Stop the old proxy, choose another port, and check the listener host.</p>
        </details>
        <details class="help-detail">
          <summary>WebSocket blocks on ws://</summary>
          <p>Browsers block insecure WebSockets from HTTPS pages. Use wss:// or open the workbench over plain HTTP on localhost.</p>
        </details>
      </section>
    </div>

    <section class="card cli-card">
      <header class="card__head"><div><span class="eyebrow">CLI</span><h3>Useful flags</h3></div><Icon name="terminal" :size="18" class="muted" /></header>
      <div class="cli-grid mono">
        <span>--host &lt;addr&gt;</span><span>Listen address, default 127.0.0.1</span>
        <span>--port &lt;port&gt;</span><span>Workbench port, default 8787</span>
        <span>--data-dir &lt;path&gt;</span><span>SQLite and runtime data</span>
        <span>--web-root &lt;path&gt;</span><span>Built frontend directory</span>
        <span>--tls-cert / --tls-key</span><span>Serve the UI over HTTPS</span>
        <span>--self-check</span><span>Offline smoke test</span>
        <span>--version</span><span>Print version and exit</span>
      </div>
    </section>
  </div>
</template>
