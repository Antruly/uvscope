import { createRouter, createWebHistory } from 'vue-router'

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    {
      path: '/',
      name: 'dashboard',
      component: () => import('./views/DashboardView.vue'),
      meta: { title: 'Overview', titleKey: 'nav.overview', icon: 'dashboard', componentName: 'DashboardView' },
    },
    {
      path: '/http',
      name: 'http',
      component: () => import('./views/HttpView.vue'),
      meta: { title: 'HTTP Studio', titleKey: 'nav.http', icon: 'bolt', componentName: 'HttpView', module: 'http' },
    },
    {
      path: '/websocket',
      name: 'websocket',
      component: () => import('./views/WebSocketView.vue'),
      meta: { title: 'WebSocket', titleKey: 'nav.websocket', icon: 'pulse', componentName: 'WebSocketView', module: 'websocket' },
    },
    {
      path: '/transport',
      name: 'transport',
      component: () => import('./views/TransportView.vue'),
      meta: { title: 'TCP / UDP', titleKey: 'nav.transport', icon: 'layers', componentName: 'TransportView', module: 'tcpudp' },
    },
    {
      path: '/listeners',
      name: 'listeners',
      component: () => import('./views/ListenersView.vue'),
      meta: { title: 'TCP / UDP Servers', titleKey: 'nav.listeners', icon: 'server', componentName: 'ListenersView', module: 'tcpudp' },
    },
    {
      path: '/socks5',
      name: 'socks5',
      component: () => import('./views/Socks5View.vue'),
      meta: { title: 'SOCKS5 Server', titleKey: 'nav.socks5', icon: 'key', componentName: 'Socks5View', module: 'tcpudp' },
    },
    {
      path: '/terminal',
      name: 'terminal',
      component: () => import('./views/TerminalView.vue'),
      meta: { title: 'Terminal', titleKey: 'nav.terminal', icon: 'terminal', componentName: 'TerminalView', module: 'terminal' },
    },
    {
      path: '/soap',
      name: 'soap',
      component: () => import('./views/SoapView.vue'),
      meta: { title: 'SOAP / WSDL', titleKey: 'nav.soap', icon: 'braces', componentName: 'SoapView', module: 'soap' },
    },
    {
      path: '/tls',
      name: 'tls',
      component: () => import('./views/TlsView.vue'),
      meta: { title: 'TLS Inspector', titleKey: 'nav.tls', icon: 'shield', componentName: 'TlsView', module: 'tls' },
    },
    {
      path: '/proxy',
      name: 'proxy',
      component: () => import('./views/ProxyView.vue'),
      meta: { title: 'Reverse Proxy', titleKey: 'nav.proxy', icon: 'route', componentName: 'ProxyView', module: 'proxy' },
    },
    {
      path: '/users',
      name: 'users',
      component: () => import('./views/UsersView.vue'),
      meta: { title: 'Users & Roles', titleKey: 'nav.users', icon: 'users', componentName: 'UsersView', admin: 'users' },
    },
    {
      path: '/settings',
      name: 'settings',
      component: () => import('./views/SettingsView.vue'),
      meta: { title: 'Settings', titleKey: 'nav.settings', icon: 'settings', componentName: 'SettingsView', admin: 'settings' },
    },
    {
      path: '/about',
      name: 'about',
      component: () => import('./views/AboutView.vue'),
      meta: { title: 'About', titleKey: 'nav.about', icon: 'spark', componentName: 'AboutView' },
    },
    {
      path: '/help',
      name: 'help',
      component: () => import('./views/HelpView.vue'),
      meta: { title: 'Help', titleKey: 'nav.help', icon: 'help', componentName: 'HelpView' },
    },
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],
  scrollBehavior() {
    return { top: 0 }
  },
})
