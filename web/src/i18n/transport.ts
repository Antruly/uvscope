const en: Record<string, string> = {
  'transport.notify.hostPortRequired': 'Host and port are required',
  'transport.notify.probeFailed': 'Probe failed',

  'transport.header.eyebrow': 'Transport bench',
  'transport.header.title': 'Bytes, without the ceremony.',
  'transport.header.lead': 'Open a one-shot TCP connection or fire a UDP datagram through the libuvcpp net layer.',

  'transport.form.client': '{mode} client',
  'transport.form.title': 'One-shot probe',
  'transport.form.host': 'Host',
  'transport.form.port': 'Port',
  'transport.form.encoding': 'Payload encoding',
  'transport.form.encodingText': 'UTF-8 text',
  'transport.form.encodingHex': 'Hex bytes',
  'transport.form.connectTimeout': 'Connect timeout',
  'transport.form.readTimeout': 'Read timeout',
  'transport.form.receiveTimeout': 'Receive timeout',
  'transport.form.payload': 'Payload',
  'transport.form.payloadPlaceholder': 'Bytes to send…',
  'transport.form.meta': '{count} chars · encrypted transport is configured per endpoint',
  'transport.form.probing': 'Probing…',
  'transport.form.openTcp': 'Open TCP connection',
  'transport.form.sendDatagram': 'Send datagram',
  'transport.form.info':
    'Persistent TCP/UDP listeners and raw QUIC streams are planned for 0.3. This bench is a one-shot probe.',

  'transport.result.eyebrow': 'Result',
  'transport.result.title': 'Wire transcript',
  'transport.result.complete': 'COMPLETE',
  'transport.result.failed': 'FAILED',

  'transport.empty.title': 'No wire data yet',
  'transport.empty.body': 'Send a probe to inspect raw bytes, timing and the peer response.',

  'transport.meta.target': 'Target',
  'transport.meta.elapsed': 'Elapsed',
  'transport.meta.connect': 'Connect',
  'transport.meta.read': 'Read',
  'transport.meta.timedOut': 'timed out',
  'transport.meta.complete': 'complete',

  'transport.wire.sent': 'Sent',
  'transport.wire.received': 'Received',
  'transport.wire.emptyPayload': '(empty payload)',
  'transport.wire.noDatagram': 'No datagram before the receive timeout.',
  'transport.wire.noBytes': '(no bytes)',

  'transport.hex.noBytes': 'No bytes to display.',
}

const zh: Record<string, string> = {
  'transport.notify.hostPortRequired': '请填写主机和端口',
  'transport.notify.probeFailed': '探测失败',

  'transport.header.eyebrow': '传输测试台',
  'transport.header.title': '直接看字节，不绕弯子。',
  'transport.header.lead': '通过 libuvcpp 网络层发起一次性 TCP 连接或发送 UDP 数据报。',

  'transport.form.client': '{mode} 客户端',
  'transport.form.title': '一次性探测',
  'transport.form.host': '主机',
  'transport.form.port': '端口',
  'transport.form.encoding': '载荷编码',
  'transport.form.encodingText': 'UTF-8 文本',
  'transport.form.encodingHex': 'Hex 字节',
  'transport.form.connectTimeout': '连接超时',
  'transport.form.readTimeout': '读取超时',
  'transport.form.receiveTimeout': '接收超时',
  'transport.form.payload': '载荷',
  'transport.form.payloadPlaceholder': '待发送字节…',
  'transport.form.meta': '{count} 字符 · 加密传输按端点配置',
  'transport.form.probing': '正在探测…',
  'transport.form.openTcp': '建立 TCP 连接',
  'transport.form.sendDatagram': '发送数据报',
  'transport.form.info': '持久化 TCP/UDP 监听与原始 QUIC 流计划在 0.3 提供。此测试台为一次性探测。',

  'transport.result.eyebrow': '结果',
  'transport.result.title': '线路记录',
  'transport.result.complete': '完成',
  'transport.result.failed': '失败',

  'transport.empty.title': '暂无线路数据',
  'transport.empty.body': '发送一次探测，检查原始字节、耗时与对端响应。',

  'transport.meta.target': '目标',
  'transport.meta.elapsed': '耗时',
  'transport.meta.connect': '连接',
  'transport.meta.read': '读取',
  'transport.meta.timedOut': '超时',
  'transport.meta.complete': '完成',

  'transport.wire.sent': '已发送',
  'transport.wire.received': '已接收',
  'transport.wire.emptyPayload': '（空载荷）',
  'transport.wire.noDatagram': '接收超时前未收到数据报。',
  'transport.wire.noBytes': '（无字节）',

  'transport.hex.noBytes': '无字节可显示。',
}

export default { en, zh }
