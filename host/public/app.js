const paths = {
  grid: '<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>',
  device: '<rect x="6" y="2" width="12" height="20" rx="3"/><path d="M10 18h4M9 7l2 2-2 2m6-4-2 2 2 2"/>',
  terminal: '<rect x="3" y="4" width="18" height="16" rx="3"/><path d="m7 9 3 3-3 3m6 0h4"/>',
  monitor: '<rect x="3" y="3" width="18" height="13" rx="2"/><path d="M8 21h8m-4-5v5"/>',
  cloud: '<path d="M7 18h11a4 4 0 0 0 .4-8 6.5 6.5 0 0 0-12.7-1A4.5 4.5 0 0 0 7 18Z"/>',
  bluetooth: '<path d="m7 6 10 12-5 4V2l5 4L7 18"/>',
  wifi: '<path d="M2 8a16 16 0 0 1 20 0M5 12a11 11 0 0 1 14 0m-11 4a6 6 0 0 1 8 0m-4 4h0"/>',
  play: '<path d="m8 4 12 8-12 8Z"/>',
  stop: '<rect x="6" y="6" width="12" height="12" rx="1"/>',
  refresh: '<path d="M20 7v5h-5M4 17v-5h5m10-5A8 8 0 0 0 5 6m0 11a8 8 0 0 0 14 1"/>',
  settings: '<path d="M4 7h16M4 17h16"/><circle cx="9" cy="7" r="3" fill="currentColor" stroke="none"/><circle cx="15" cy="17" r="3" fill="currentColor" stroke="none"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  arrow: '<path d="M5 12h14m-5-5 5 5-5 5"/>',
  external: '<path d="M14 3h7v7m0-7L11 13m-2-8H5a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-4"/>',
  activity: '<path d="M2 12h4l3-8 6 16 3-8h4"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  close: '<path d="m6 6 12 12M6 18 18 6"/>',
  battery: '<rect x="2" y="6" width="17" height="12" rx="2"/><path d="M22 10v4M6 10v4m4-4v4m4-4v4"/>',
  bolt: '<path d="m13 2-8 12h6l-1 8 9-13h-6Z"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6m0-10h0"/>',
}
const icon = name => `<svg class="icon" viewBox="0 0 24 24" aria-hidden="true">${paths[name] || paths.device}</svg>`
const escape = value => String(value ?? '').replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char])
const $ = selector => document.querySelector(selector)
const face = '<svg class="device-face" viewBox="0 0 52 38" aria-hidden="true"><path d="m7 8 10 6-10 6m38-12-10 6 10 6M22 28l10-1"/></svg>'
const labels = { stopped: '已停止', running: '运行中', starting: '启动中', stopping: '停止中', error: '启动异常', unhealthy: '等待恢复', external: '外部运行' }
const pageNames = { overview: '服务总览', devices: '我的设备', logs: '运行日志' }
let state = null, csrf = '', offline = false, polling = false, page = 'overview', editing = null
let logService = '', logSearch = '', toastTimer
const pending = new Set()
const kindIcon = kind => kind === 'gateway' ? 'cloud' : kind === 'ble' ? 'bluetooth' : 'wifi'
const servicesFor = device => state.services.filter(item => item.deviceId === device.id)
const serviceName = service => {
  if (service === 'manager') return '管理后台'
  const item = state?.services.find(item => item.id === service)
  const device = state?.devices.find(device => device.id === item?.deviceId)
  return device ? `${device.name} · ${item.kind === 'ble' ? '蓝牙' : 'Wi-Fi'}` : item?.name || service
}
const time = value => new Date(value).toLocaleTimeString('zh-CN', { hour12: false })
function uptime(value) {
  if (!value) return '—'
  const seconds = Math.max(0, Math.floor((Date.now() - value) / 1000))
  return seconds >= 3600 ? `${Math.floor(seconds / 3600)} 小时 ${Math.floor(seconds % 3600 / 60)} 分` : seconds >= 60 ? `${Math.floor(seconds / 60)} 分 ${seconds % 60} 秒` : `${seconds} 秒`
}
function statusLabel(service) {
  if (service.kind === 'ble' && service.state === 'running') {
    if (service.voiceReady && service.connected) return '语音就绪'
    if (service.connected) return '设备已连接'
    if (['unauthorized', 'unsupported', 'powered-off'].includes(service.bleState)) return '蓝牙不可用'
    return '搜索设备中'
  }
  return labels[service.state] || service.state
}
function badge(service) {
  const color = ['running', 'external'].includes(service.state) ? 'green' : ['starting', 'stopping', 'unhealthy'].includes(service.state) ? 'amber' : service.state === 'error' ? 'red' : ''
  return `<span class="badge ${escape(service.state)}"><span class="dot ${color}"></span>${escape(statusLabel(service))}</span>`
}
function button(service, action, title, compact = false) {
  const busy = pending.has(service.id) || pending.has('all') || ['starting', 'stopping'].includes(service.state)
  const disabled = offline || busy || (service.state === 'external' && action !== 'start')
  return `<button class="${compact ? 'icon-button' : `button ${action === 'start' ? 'secondary' : 'stop-button'}`}" data-command="${action}" data-service="${escape(service.id)}" data-focus-key="${escape(service.id)}-${action}" title="${escape(title)}" aria-label="${escape(title)}" ${disabled ? 'disabled' : ''}>${icon(action === 'restart' ? 'refresh' : action === 'stop' ? 'stop' : 'play')}${compact ? '' : title}</button>`
}
function serviceRow(service) {
  const device = state.devices.find(device => device.id === service.deviceId)
  const subtitle = service.kind === 'gateway' ? '语音模型与会话网关' : `${device?.name} · ${service.kind === 'ble' ? device?.bleName || '尚未绑定蓝牙设备' : `端口 ${device?.bridgePort}`}`
  let detail = service.kind === 'gateway' ? service.health?.model || '等待启动' : service.kind === 'ble' ? service.device?.firmware ? `固件 ${service.device.firmware}` : '等待设备连接' : 'PCM 音频备用链路'
  if (service.lastError) detail = service.lastError
  const running = service.owned || service.state === 'external'
  return `<div class="service-row"><div class="service-identity"><span class="service-icon ${service.kind}">${icon(kindIcon(service.kind))}</span><div><h3>${escape(service.name)}</h3><div class="service-subtitle">${escape(subtitle)}</div></div></div>${badge(service)}<div class="service-detail" title="${escape(detail)}">${escape(detail.slice(0, 60))}<br><span class="mono">${service.pid ? `PID ${service.pid} · ${uptime(service.startedAt)}` : service.state === 'external' ? '由其他进程管理' : '按需启动'}</span></div><div class="service-actions">${button(service, running ? 'stop' : 'start', running ? '停止' : '启动')}${button(service, 'restart', '重启', true)}<button class="icon-button" data-logs="${escape(service.id)}" title="查看日志" aria-label="查看 ${escape(subtitle)} 的日志">${icon('terminal')}</button></div></div>`
}
function deviceCard(device, detailed = false) {
  const services = servicesFor(device), ble = services.find(item => item.kind === 'ble'), bridge = services.find(item => item.kind === 'bridge')
  const connected = services.some(item => item.connected), actual = ble?.device
  const battery = actual?.battery, large = device.board === 'waveshare-lcd-3.5'
  const metrics = ble?.metrics && Date.now() - ble.metrics.at < 5000 ? ble.metrics : null
  const batteryLabel = battery?.valid && battery.percent !== null ? `${battery.percent}%` : '—'
  return `<article class="device-card"><div class="device-visual ${large ? 'large' : ''}"><span class="tag">${large ? '3.5″ / 320 × 480' : '1.54″ / 240 × 240'}</span><div class="mini-device">${face}</div><span class="visual-status"><span class="dot ${connected ? 'green' : ''}"></span>${connected ? '已连接' : '离线'}</span></div><div class="device-body"><h3>${escape(device.name)}<button class="icon-button" data-edit="${escape(device.id)}" aria-label="设置 ${escape(device.name)}" title="设备设置">${icon('settings')}</button></h3><p class="board-detail">Waveshare · ESP32-S3</p><div class="device-metadata"><span>${actual?.firmware ? `v${escape(actual.firmware)}` : '固件版本待读取'}</span>${large ? '<span>USB 供电</span>' : `<span class="battery" title="${connected ? '设备电量估算' : '上次连接的电量'}">${icon(battery?.charging ? 'bolt' : 'battery')}${connected ? '' : battery ? '上次 ' : ''}${batteryLabel}</span>`}</div><div class="device-divider"></div><div class="device-connect">${button(ble, ble.owned ? 'stop' : 'start', ble.owned ? '停止蓝牙' : '连接蓝牙')}<button class="button subtle" data-logs="${escape(ble.id)}">${icon('terminal')}日志</button></div>${detailed ? `<p class="device-note">${device.bleName ? escape(device.bleName) : '填写蓝牙名称后，即可连接这台设备。'}<br>Wi-Fi Bridge :${device.bridgePort} · ${escape(labels[bridge.state])}</p>${metrics ? `<div class="metrics-row"><span>麦克风<strong>${metrics.fps} fps</strong></span><span title="主机解码后的麦克风数据量，不是 BLE 无线链路速率">PCM 数据<strong>${metrics.kibPerSecond} KiB/s</strong></span></div>` : ''}` : ''}</div></article>`
}
function recentFeed() {
  const entries = state.logs.slice(-3).reverse()
  if (!entries.length) return `<div class="quiet"><span class="quiet-face">- _ -</span><strong>这里安安静静的</strong><p>启动服务后，连接与运行记录会出现在这里。</p></div>`
  return entries.map(entry => `<div class="feed-line"><span class="dot ${entry.level === 'error' ? 'red' : 'green'}"></span><div class="feed-content"><strong title="${escape(entry.message)}">${escape(entry.message)}</strong><span>${escape(serviceName(entry.service))}</span></div><time>${time(entry.time)}</time></div>`).join('')
}
function pageHeader(title, subtitle, actions = '') {
  return `<div class="page-title"><div><span class="eyebrow">YOUR LITTLE CONTROL ROOM</span><h1>${title}</h1><p class="subtitle">${subtitle}</p></div><div class="actions">${actions}</div></div>`
}
function overview() {
  const running = state.services.filter(item => ['running', 'external'].includes(item.state)).length
  const connected = state.devices.filter(device => servicesFor(device).some(item => item.connected)).length
  const gateway = state.services.find(item => item.kind === 'gateway')
  const busy = pending.size || offline
  const gatewayLink = gateway.health ? `<a class="button secondary" href="${escape(gateway.url)}" target="_blank" rel="noopener noreferrer">${icon('external')}对话控制台</a>` : ''
  const actions = `${gatewayLink}<button class="button secondary" data-command="stop" data-service="all" ${busy || !state.services.some(item => item.owned) ? 'disabled' : ''}>${icon('stop')}全部停止</button><button class="button primary" data-command="start" data-service="all" ${busy ? 'disabled' : ''}>${icon('play')}${pending.has('all') ? '正在处理…' : '一键启动'}</button>`
  const stat = (title, value, caption, symbol, word = false) => `<div class="stat"><div class="stat-top">${title}${icon(symbol)}</div><div class="stat-value ${word ? 'word' : ''}">${value}</div><div class="stat-description" title="${escape(caption)}">${escape(caption)}</div></div>`
  const setup = !state.setup.hostConfigured || !state.setup.bleBuilt || !state.setup.gatewayInstalled
    ? `<div class="banner setup-banner">${!state.setup.hostConfigured ? '主机配置尚未完成，请先运行 make setup。 ' : ''}${!state.setup.bleBuilt ? '蓝牙程序尚未编译，可运行 make ble-build。 ' : ''}${!state.setup.gatewayInstalled ? 'Qwen 依赖尚未安装。' : ''}</div>` : ''
  return pageHeader('一切连接，都在这里。', '查看服务、连接设备，让 JUFF 随时准备好与你说话。', actions) + setup
    + `<section class="stats" aria-label="运行概况">${stat('运行中的服务', `${running}<small>/ ${state.services.length}</small>`, '独立启停，状态实时更新', 'activity')}${stat('已连接设备', `${connected}<small>/ ${state.devices.length}</small>`, '每台设备拥有独立的连接', 'device')}${stat('语音网关', gateway.health ? '已就绪' : '未启动', gateway.health?.model || '等待启动 Qwen Gateway', 'cloud', true)}${stat('后台运行时间', uptime(state.startedAt), '服务管理后台独立运行', 'clock', true)}</section>`
    + `<div class="overview-columns"><section class="overview-services"><div class="section-header"><h2>服务状态 <small>${state.services.length} 个服务</small></h2><span class="text-link">${icon('activity')}实时更新</span></div><section class="service-list" aria-label="服务列表">${state.services.map(serviceRow).join('')}</section><p class="overview-hint">${icon('info')}一键启动网关与已绑定设备的蓝牙；Wi-Fi Bridge 可按需单独开启。停止网关会同时停止关联服务。</p>`
    + `</section><div class="overview-aside"><section><div class="section-header"><h2>我的小伙伴 <small>${state.devices.length} 台设备</small></h2><a class="text-link" href="#devices">管理设备${icon('arrow')}</a></div><div class="device-grid">${state.devices.slice(0, 2).map(device => deviceCard(device)).join('') || '<div class="quiet"><p>添加第一台 JUFF，开始连接。</p></div>'}</div></section><section><div class="section-header"><h2>最近动态</h2><a class="text-link" href="#logs">全部日志${icon('arrow')}</a></div><div class="feed">${recentFeed()}</div></section></div></div>`
}
function devicesPage() {
  return pageHeader('我的小伙伴', '大小屏各有自己的配置，连接状态在这里一目了然。', `<button class="button primary" data-action="add-device">${icon('plus')}添加设备</button>`)
    + `<div class="devices-page-grid">${state.devices.map(device => deviceCard(device, true)).join('')}</div>${!state.devices.length ? '<div class="quiet"><span class="quiet-face">&gt;_&lt;</span><strong>添加你的第一台 JUFF</strong><p>支持 1.54 英寸与 3.5 英寸两款硬件。</p></div>' : ''}`
}
function logsPage() {
  return pageHeader('每一次连接，都有迹可循。', '查看服务启停、连接变化与错误信息。日志只保留最近 600 条运行记录。')
    + `<section class="log-panel"><div class="log-toolbar"><input id="log-search" data-focus-key="log-search" aria-label="搜索日志" placeholder="搜索日志内容…" value="${escape(logSearch)}"><select id="log-service" data-focus-key="log-service" aria-label="按服务筛选"><option value="">全部服务</option><option value="manager" ${logService === 'manager' ? 'selected' : ''}>管理后台</option>${state.services.map(item => `<option value="${escape(item.id)}" ${logService === item.id ? 'selected' : ''}>${escape(serviceName(item.id))}</option>`).join('')}</select><span class="log-count" id="log-count"></span></div><div class="log-rows" id="log-rows"></div></section>`
}
let fullLogs = []
function renderLogs() {
  if (page !== 'logs' || !$('#log-rows')) return
  const entries = fullLogs.filter(entry => (!logService || entry.service === logService) && (!logSearch || entry.message.toLowerCase().includes(logSearch.toLowerCase())))
  $('#log-count').textContent = `${entries.length} 条记录`
  $('#log-rows').innerHTML = entries.length ? entries.slice().reverse().map(entry => `<div class="log-row ${escape(entry.level)}"><time>${time(entry.time)}</time><span class="log-level ${escape(entry.level)}">${entry.level === 'error' ? 'ERROR' : 'INFO'}</span><span class="log-source" title="${escape(serviceName(entry.service))}">${escape(serviceName(entry.service))}</span><span class="log-message">${escape(entry.message)}</span></div>`).join('') : '<div class="quiet"><span class="quiet-face">- _ -</span><strong>暂无匹配记录</strong><p>服务产生新事件时会自动更新。</p></div>'
}
function render() {
  if (!state) return
  const focused = document.activeElement, focusKey = focused?.dataset.focusKey, position = focused?.selectionStart
  $('#workspace').innerHTML = page === 'devices' ? devicesPage() : page === 'logs' ? logsPage() : overview()
  $('#nav-count').textContent = state.devices.length
  $('#breadcrumb-page').textContent = pageNames[page]
  document.querySelectorAll('[data-page]').forEach(link => link.classList.toggle('active', link.dataset.page === page))
  $('#last-updated').textContent = `最近更新 ${time(state.updatedAt)} · 每 2 秒刷新`
  if (focusKey) {
    const element = [...document.querySelectorAll('[data-focus-key]')].find(item => item.dataset.focusKey === focusKey)
    element?.focus({ preventScroll: true })
    if (position !== null && element?.setSelectionRange && element.type !== 'number') element.setSelectionRange(position, position)
  }
  renderLogs()
}
function notify(message, error = false) {
  const toast = $('#toast'); toast.textContent = message; toast.className = `toast${error ? ' error' : ''}`
  clearTimeout(toastTimer); toastTimer = setTimeout(() => toast.classList.add('hidden'), error ? 6500 : 3500)
}
async function request(url, method = 'GET', data) {
  const response = await fetch(url, { method, headers: method === 'GET' ? {} : { 'content-type': 'application/json', 'x-juff-csrf': csrf }, ...(data === undefined ? {} : { body: JSON.stringify(data) }) })
  const result = await response.json()
  if (!response.ok) throw new Error(result.error || '请求失败')
  return result
}
async function poll() {
  if (polling) return
  polling = true
  try {
    const result = await request('/api/state'); csrf = result.csrfToken; state = result; offline = false
    if (page === 'logs') fullLogs = (await request('/api/logs')).entries
    $('#connection-status').innerHTML = '<span class="dot green"></span>本机后台已连接'
    $('#connection-error').classList.add('hidden')
    render()
  } catch {
    offline = true
    $('#connection-status').innerHTML = '<span class="dot red"></span>连接中断'
    $('#connection-error').textContent = '暂时连接不到管理后台，正在重试。可以在终端运行 make admin 重新启动。'
    $('#connection-error').classList.remove('hidden')
    render()
  } finally { polling = false }
}
function openDevice(id = null) {
  if (!state) return
  editing = id
  const used = new Set(state.devices.map(device => device.bridgePort))
  let port = 8765; while (used.has(port)) port++
  const profile = state.devices.find(item => item.id === id) || { name: '', board: 'waveshare-lcd-1.54', bleName: '', bridgePort: port, sessionId: `esp32-${crypto.randomUUID().slice(0, 8)}` }
  const form = $('#device-form')
  for (const key of ['name', 'board', 'bleName', 'bridgePort', 'sessionId']) form.elements[key].value = profile[key]
  $('#dialog-title').textContent = id ? '设备设置' : '添加一个小伙伴'
  $('#delete-device').classList.toggle('hidden', !id)
  $('#delete-device').textContent = '移除设备'
  $('#form-error').classList.add('hidden')
  $('#device-dialog').showModal()
}
document.addEventListener('click', async event => {
  const button = event.target.closest('button')
  if (!button || button.disabled) return
  if (button.dataset.action === 'close-dialog') return $('#device-dialog').close()
  if (button.dataset.action === 'add-device') return openDevice()
  if (button.dataset.edit) return openDevice(button.dataset.edit)
  if (button.dataset.logs) { logService = button.dataset.logs; location.hash = 'logs'; if (page === 'logs') { render(); void poll() }; return }
  if (button.dataset.command) {
    const id = button.dataset.service, action = button.dataset.command
    pending.add(id); render()
    try {
      state = await request(`/api/services/${encodeURIComponent(id)}/${action}`, 'POST', {})
      notify(action === 'stop' ? '服务已停止' : action === 'restart' ? '服务已重启' : '服务已启动；设备连接状态会自动更新')
    } catch (error) { notify(error.message, true) }
    finally { pending.delete(id); render(); void poll() }
  }
})
$('#device-form').addEventListener('submit', async event => {
  event.preventDefault()
  const form = event.currentTarget, data = Object.fromEntries(new FormData(form))
  data.bridgePort = Number(data.bridgePort); data.bleName = data.bleName.trim()
  $('#save-device').disabled = true; $('#form-error').classList.add('hidden')
  try {
    state = await request(editing ? `/api/devices/${editing}` : '/api/devices', editing ? 'PUT' : 'POST', data)
    $('#device-dialog').close(); notify('设备配置已保存'); render()
  } catch (error) { $('#form-error').textContent = error.message; $('#form-error').classList.remove('hidden') }
  finally { $('#save-device').disabled = false }
})
$('#delete-device').addEventListener('click', async () => {
  if (!editing) return
  const button = $('#delete-device')
  if (button.textContent !== '确认移除') { button.textContent = '确认移除'; return }
  button.disabled = true
  try {
    state = await request(`/api/devices/${editing}`, 'DELETE', {})
    $('#device-dialog').close(); notify('设备已移除'); render()
  } catch (error) { $('#form-error').textContent = error.message; $('#form-error').classList.remove('hidden') }
  finally { button.disabled = false }
})
document.addEventListener('input', event => { if (event.target.id === 'log-search') { logSearch = event.target.value; renderLogs() } })
document.addEventListener('change', event => { if (event.target.id === 'log-service') { logService = event.target.value; renderLogs() } })
function navigate() { page = Object.hasOwn(pageNames, location.hash.slice(1)) ? location.hash.slice(1) : 'overview'; render(); void poll() }
document.querySelectorAll('[data-icon]').forEach(element => { element.innerHTML = icon(element.dataset.icon) })
window.addEventListener('hashchange', navigate)
navigate()
setInterval(poll, 2000)
