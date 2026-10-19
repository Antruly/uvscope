const en: Record<string, string> = {
  'users.eyebrow': 'Access control',
  'users.title': 'People, roles, blast radius.',
  'users.lead': 'Provision local accounts and keep administrative surfaces limited to the right people.',
  'users.new': 'New user',

  'users.roles.admin.title': 'Administrator',
  'users.roles.admin.text': 'Users, settings, reset, every debug module and gateway.',
  'users.roles.operator.title': 'Operator',
  'users.roles.operator.text': 'Debug modules, history and reverse proxy management.',
  'users.roles.viewer.title': 'Viewer',
  'users.roles.viewer.text': 'Read-only dashboard, history and about pages.',

  'users.directory.eyebrow': 'Directory',
  'users.directory.count': '{count} local accounts',
  'users.loading': 'Loading users…',

  'users.table.user': 'User',
  'users.table.role': 'Role',
  'users.table.status': 'Status',
  'users.table.lastLogin': 'Last login',

  'users.row.you': 'you',
  'users.row.created': '#{id} · created {date}',
  'users.row.never': 'never',
  'users.status.enabled': 'enabled',
  'users.status.disabled': 'disabled',

  'users.action.resetPassword': 'Reset password',
  'users.action.deleteUser': 'Delete user',

  'users.modal.eyebrow': 'Local account',
  'users.modal.createTitle': 'Create user',
  'users.modal.username': 'Username',
  'users.modal.usernamePlaceholder': 'alice',
  'users.modal.tempPassword': 'Temporary password',
  'users.modal.passwordPlaceholder': 'At least 8 characters',
  'users.modal.role': 'Role',
  'users.modal.roleViewer': 'Viewer — read only',
  'users.modal.roleOperator': 'Operator — debug and gateway',
  'users.modal.roleAdmin': 'Administrator — full control',
  'users.modal.cancel': 'Cancel',
  'users.modal.create': 'Create user',

  'users.reset.eyebrow': 'Credential reset',
  'users.reset.newPassword': 'New password',
  'users.reset.warning': 'All existing sessions for this user will be invalidated.',
  'users.reset.submit': 'Reset password',

  'users.error.load': 'Could not load users',
  'users.error.usernameShort': 'Username must be at least 3 characters.',
  'users.error.passwordShort': 'Password must be at least 8 characters.',
  'users.error.updateRole': 'Could not update role',
  'users.error.resetFailed': 'Password reset failed',

  'users.notify.created': 'User created',
  'users.notify.roleUpdated': 'Role updated',
  'users.notify.roleUpdatedBody': '{username} is now {role}',
  'users.notify.enabled': 'User enabled',
  'users.notify.disabled': 'User disabled',
  'users.notify.deleted': 'User deleted',
  'users.notify.passwordShort': 'Password must be at least 8 characters',
  'users.notify.reset': 'Password reset',
  'users.notify.resetBody': 'Existing sessions for {username} were invalidated.',

  'users.confirm.delete': 'Delete user “{username}”? This cannot be undone.',
}

const zh: Record<string, string> = {
  'users.eyebrow': '访问控制',
  'users.title': '人员、角色、影响范围。',
  'users.lead': '创建本地账号，把管理能力限定在合适的人手中。',
  'users.new': '新建用户',

  'users.roles.admin.title': '管理员',
  'users.roles.admin.text': '用户、设置、重置，以及全部调试模块与网关。',
  'users.roles.operator.title': '操作员',
  'users.roles.operator.text': '调试模块、历史记录与反向代理管理。',
  'users.roles.viewer.title': '只读用户',
  'users.roles.viewer.text': '只读的概览、历史与关于页面。',

  'users.directory.eyebrow': '目录',
  'users.directory.count': '{count} 个本地账号',
  'users.loading': '正在加载用户…',

  'users.table.user': '用户',
  'users.table.role': '角色',
  'users.table.status': '状态',
  'users.table.lastLogin': '最近登录',

  'users.row.you': '你',
  'users.row.created': '#{id} · 创建于 {date}',
  'users.row.never': '从未登录',
  'users.status.enabled': '已启用',
  'users.status.disabled': '已禁用',

  'users.action.resetPassword': '重置密码',
  'users.action.deleteUser': '删除用户',

  'users.modal.eyebrow': '本地账号',
  'users.modal.createTitle': '创建用户',
  'users.modal.username': '用户名',
  'users.modal.usernamePlaceholder': '例如 alice',
  'users.modal.tempPassword': '临时密码',
  'users.modal.passwordPlaceholder': '至少 8 个字符',
  'users.modal.role': '角色',
  'users.modal.roleViewer': '只读用户 — 仅可查看',
  'users.modal.roleOperator': '操作员 — 调试与网关',
  'users.modal.roleAdmin': '管理员 — 完全控制',
  'users.modal.cancel': '取消',
  'users.modal.create': '创建用户',

  'users.reset.eyebrow': '凭据重置',
  'users.reset.newPassword': '新密码',
  'users.reset.warning': '该用户的所有现有会话都将失效。',
  'users.reset.submit': '重置密码',

  'users.error.load': '无法加载用户列表',
  'users.error.usernameShort': '用户名至少 3 个字符。',
  'users.error.passwordShort': '密码至少 8 个字符。',
  'users.error.updateRole': '无法更新角色',
  'users.error.resetFailed': '密码重置失败',

  'users.notify.created': '用户已创建',
  'users.notify.roleUpdated': '角色已更新',
  'users.notify.roleUpdatedBody': '{username} 的角色已设为 {role}',
  'users.notify.enabled': '用户已启用',
  'users.notify.disabled': '用户已禁用',
  'users.notify.deleted': '用户已删除',
  'users.notify.passwordShort': '密码至少 8 个字符',
  'users.notify.reset': '密码已重置',
  'users.notify.resetBody': '{username} 的现有会话已失效。',

  'users.confirm.delete': '删除用户“{username}”？此操作无法撤销。',
}

export default { en, zh }
