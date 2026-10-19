import { createRouter, createWebHistory } from 'vue-router'

export const router = createRouter({
  history: createWebHistory(),
  routes: [
    {
      path: '/',
      name: 'dashboard',
      component: () => import('./views/DashboardView.vue'),
      meta: { title: 'Overview', icon: 'dashboard', componentName: 'DashboardView' },
    },
    {
      path: '/http',
      name: 'http',
      component: () => import('./views/HttpView.vue'),
      meta: { title: 'HTTP Studio', icon: 'bolt', componentName: 'HttpView', module: 'http' },
    },
    {
      path: '/websocket',
      name: 'websocket',
      component: () => import('./views/WebSocketView.vue'),
      meta: { title: 'WebSocket', icon: 'pulse', componentName: 'WebSocketView', module: 'websocket' },
    },
    {
      path: '/transport',
      name: 'transport',
      component: () => import('./views/TransportView.vue'),
      meta: { title: 'TCP / UDP', icon: 'layers', componentName: 'TransportView', module: 'tcpudp' },
    },
    {
      path: '/soap',
      name: 'soap',
      component: () => import('./views/SoapView.vue'),
      meta: { title: 'SOAP / WSDL', icon: 'braces', componentName: 'SoapView', module: 'soap' },
    },
    {
      path: '/tls',
      name: 'tls',
      component: () => import('./views/TlsView.vue'),
      meta: { title: 'TLS Inspector', icon: 'shield', componentName: 'TlsView', module: 'tls' },
    },
    {
      path: '/proxy',
      name: 'proxy',
      component: () => import('./views/ProxyView.vue'),
      meta: { title: 'Reverse Proxy', icon: 'route', componentName: 'ProxyView', module: 'proxy' },
    },
    {
      path: '/users',
      name: 'users',
      component: () => import('./views/UsersView.vue'),
      meta: { title: 'Users & Roles', icon: 'users', componentName: 'UsersView', admin: 'users' },
    },
    {
      path: '/settings',
      name: 'settings',
      component: () => import('./views/SettingsView.vue'),
      meta: { title: 'Settings', icon: 'settings', componentName: 'SettingsView', admin: 'settings' },
    },
    {
      path: '/about',
      name: 'about',
      component: () => import('./views/AboutView.vue'),
      meta: { title: 'About', icon: 'spark', componentName: 'AboutView' },
    },
    {
      path: '/help',
      name: 'help',
      component: () => import('./views/HelpView.vue'),
      meta: { title: 'Help', icon: 'help', componentName: 'HelpView' },
    },
    { path: '/:pathMatch(.*)*', redirect: '/' },
  ],
  scrollBehavior() {
    return { top: 0 }
  },
})
