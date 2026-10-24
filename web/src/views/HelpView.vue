<script setup lang="ts">
import Icon from '../components/Icon.vue'
import { useAuthStore } from '../stores/auth'
import { useI18nStore } from '../stores/i18n'
import { useRouter } from 'vue-router'

defineOptions({ name: 'HelpView' })
const auth = useAuthStore()
const router = useRouter()
const i18n = useI18nStore()
const t = i18n.t

const modules = [
  { icon: 'bolt', titleKey: 'help.modules.http.title', textKey: 'help.modules.http.text' },
  { icon: 'pulse', titleKey: 'help.modules.websocket.title', textKey: 'help.modules.websocket.text' },
  { icon: 'layers', titleKey: 'help.modules.tcpudp.title', textKey: 'help.modules.tcpudp.text' },
  { icon: 'server', titleKey: 'help.modules.listeners.title', textKey: 'help.modules.listeners.text' },
  { icon: 'key', titleKey: 'help.modules.socks5.title', textKey: 'help.modules.socks5.text' },
  { icon: 'terminal', titleKey: 'help.modules.terminal.title', textKey: 'help.modules.terminal.text' },
  { icon: 'eye', titleKey: 'help.modules.preview.title', textKey: 'help.modules.preview.text' },
  { icon: 'braces', titleKey: 'help.modules.soap.title', textKey: 'help.modules.soap.text' },
  { icon: 'shield', titleKey: 'help.modules.tls.title', textKey: 'help.modules.tls.text' },
  { icon: 'route', titleKey: 'help.modules.proxy.title', textKey: 'help.modules.proxy.text' },
]

const shortcuts = [
  ['Ctrl / ⌘ + Enter', 'help.shortcut.send'],
  ['Right-click a tab', 'help.shortcut.tabs'],
  ['Click a history row', 'help.shortcut.history'],
  ['Ctrl / ⌘ + K', 'help.shortcut.address'],
]
</script>

<template>
  <div class="page help-page">
    <header class="page-head">
      <div>
        <span class="eyebrow">{{ t('help.eyebrow') }}</span>
        <h2>{{ t('help.title') }}</h2>
        <p>{{ t('help.lead') }}</p>
      </div>
      <div class="page-head__actions">
        <button class="button" @click="router.push('/http')"><Icon name="bolt" :size="16" />{{ t('help.openHttp') }}</button>
      </div>
    </header>

    <section class="help-hero">
      <div class="help-hero__copy">
        <span class="eyebrow">{{ t('help.quickStart') }}</span>
        <h3>{{ t('help.threeMoves') }}</h3>
        <ol class="quick-steps">
          <li><span>1</span><div><strong>{{ t('help.step1.title') }}</strong><p>{{ t('help.step1.text') }}</p></div></li>
          <li><span>2</span><div><strong>{{ t('help.step2.title') }}</strong><p>{{ t('help.step2.text') }}</p></div></li>
          <li><span>3</span><div><strong>{{ t('help.step3.title') }}</strong><p>{{ t('help.step3.text') }}</p></div></li>
        </ol>
      </div>
      <div class="help-hero__terminal">
        <div class="terminal-bar"><i /><i /><i /><span class="mono">uvscope --port 8787</span></div>
        <pre><span class="t-dim">$</span> uvscope --port 8787
<span class="t-accent">uvscope {{ auth.version }}</span>
  workbench : http://127.0.0.1:8787
  data      : {{ auth.dataDir }}
  libuvcpp  : {{ auth.libuvcpp || '1.8.0' }}
  engine    : libuv</pre>
        <div class="terminal-foot"><span class="status-dot" />{{ t('help.loopRunning') }}</div>
      </div>
    </section>

    <section class="module-help-grid">
      <article v-for="module in modules" :key="module.titleKey" class="card module-help">
        <span class="module-help__icon"><Icon :name="module.icon" :size="19" /></span>
        <h3>{{ t(module.titleKey) }}</h3>
        <p>{{ t(module.textKey) }}</p>
      </article>
    </section>

    <div class="help-grid">
      <section class="card">
        <header class="card__head"><div><span class="eyebrow">{{ t('help.keyboard') }}</span><h3>{{ t('help.shortcuts') }}</h3></div><Icon name="terminal" :size="18" class="muted" /></header>
        <div class="shortcut-list">
          <div v-for="row in shortcuts" :key="row[0]" class="shortcut-row"><kbd>{{ row[0] }}</kbd><span>{{ t(row[1]) }}</span></div>
        </div>
      </section>

      <section class="card">
        <header class="card__head"><div><span class="eyebrow">{{ t('help.operations') }}</span><h3>{{ t('help.localBehaviour') }}</h3></div><Icon name="database" :size="18" class="muted" /></header>
        <div class="note-list">
          <div class="note-row"><span>{{ t('help.note.storage') }}</span><strong>{{ t('help.note.storageText') }}</strong></div>
          <div class="note-row"><span>{{ t('help.note.sessions') }}</span><strong>{{ t('help.note.sessionsText') }}</strong></div>
          <div class="note-row"><span>{{ t('help.note.network') }}</span><strong>{{ t('help.note.networkText') }}</strong></div>
          <div class="note-row"><span>{{ t('help.note.isolation') }}</span><strong>{{ t('help.note.isolationText') }}</strong></div>
          <div class="note-row"><span>{{ t('help.note.preview') }}</span><strong>{{ t('help.note.previewText') }}</strong></div>
          <div class="note-row"><span>{{ t('help.note.language') }}</span><strong>{{ t('help.note.languageText') }}</strong></div>
          <div class="note-row"><span>{{ t('help.note.exposure') }}</span><strong>{{ t('help.note.exposureText') }}</strong></div>
        </div>
      </section>

      <section class="card">
        <header class="card__head"><div><span class="eyebrow">{{ t('help.troubleshooting') }}</span><h3>{{ t('help.whenStuck') }}</h3></div><Icon name="help" :size="18" class="muted" /></header>
        <details class="help-detail" open>
          <summary>{{ t('help.tls.title') }}</summary>
          <p>{{ t('help.tls.textBefore') }}<em>{{ t('help.tls.insecure') }}</em>{{ t('help.tls.textAfter') }}</p>
        </details>
        <details class="help-detail">
          <summary>{{ t('help.http2.title') }}</summary>
          <p>{{ t('help.http2.text') }}</p>
        </details>
        <details class="help-detail">
          <summary>{{ t('help.proxy.title') }}</summary>
          <p>{{ t('help.proxy.text') }}</p>
        </details>
        <details class="help-detail">
          <summary>{{ t('help.ws.title') }}</summary>
          <p>{{ t('help.ws.text') }}</p>
        </details>
      </section>
    </div>

    <section class="card cli-card">
      <header class="card__head"><div><span class="eyebrow">{{ t('help.cli') }}</span><h3>{{ t('help.flags') }}</h3></div><Icon name="terminal" :size="18" class="muted" /></header>
      <div class="cli-grid mono">
        <span>--host &lt;addr&gt;</span><span>{{ t('help.cli.host') }}</span>
        <span>--port &lt;port&gt;</span><span>{{ t('help.cli.port') }}</span>
        <span>--data-dir &lt;path&gt;</span><span>{{ t('help.cli.dataDir') }}</span>
        <span>--web-root &lt;path&gt;</span><span>{{ t('help.cli.webRoot') }}</span>
        <span>--tls-cert / --tls-key</span><span>{{ t('help.cli.tls') }}</span>
        <span>--self-check</span><span>{{ t('help.cli.selfCheck') }}</span>
        <span>--version</span><span>{{ t('help.cli.version') }}</span>
      </div>
    </section>
  </div>
</template>
