// Read-only documentation preview. Uses real UI assets and synthetic data only.
import { fileURLToPath } from 'node:url'
import { createManagerServer } from '../host/src/manager/server.mjs'
import { ManagerError, validateProfiles } from '../host/src/manager/profiles.mjs'
import { LogStore } from '../host/src/manager/logs.mjs'
import { MIAOMIAO_PROFILE, MAX_ASSISTANT_CHARS } from '../host/src/manager/assistant.mjs'

const port = Number(process.argv[2] || 3211)
if (!Number.isInteger(port) || port < 1024 || port > 65535) throw new Error('Preview port must be 1024–65535')
const startedAt = Date.now() - 4080000
const devices = validateProfiles([
  { id: 'small', name: '小屏 JUFF', board: 'waveshare-lcd-1.54', bleName: 'JUFF-DEMO-SMALL', bridgePort: 8766, sessionId: 'demo-small' },
  { id: 'large', name: '大屏 JUFF', board: 'waveshare-lcd-3.5', bleName: 'JUFF-DEMO-LARGE', bridgePort: 8765, sessionId: 'demo-large' },
])
const logs = new LogStore()
for (const [service, message] of [
  ['manager', '控制台界面演示已就绪'],
  ['gateway', 'Qwen Gateway 服务已就绪'],
  ['small-ble', '小屏 JUFF 已连接，语音就绪'],
  ['large-ble', '大屏 JUFF 已连接，语音就绪'],
  ['small-ble', '设备状态已更新 · 电池 78%'],
  ['manager', '两款硬件使用独立设备配置'],
]) logs.add(service, message)

const service = (id, kind, name, deviceId = null) => ({
  id, kind, name, deviceId, state: 'stopped', pid: null, owned: false,
  startedAt: null, lastError: null, connected: false, voiceReady: false,
  device: null, metrics: null, health: null, bleState: null, url: null,
})
const manager = {
  logs,
  async refresh() {},
  async close() {},
  async command() { throw new ManagerError('这是只读界面预览，请使用 make admin 管理实际服务', 409) },
  async updateProfiles() { throw new ManagerError('演示设备配置为只读', 409) },
  async readAssistant() { return { content: MIAOMIAO_PROFILE, template: MIAOMIAO_PROFILE, maxChars: MAX_ASSISTANT_CHARS, revision: 'demo', local: true, canReconnect: false } },
  async saveAssistant() { throw new ManagerError('演示助手设定为只读', 409) },
  snapshot() {
    const now = Date.now()
    const gateway = {
      ...service('gateway', 'gateway', 'Qwen Gateway'), state: 'running', pid: 42001,
      owned: true, startedAt, health: { ok: true, model: 'Qwen Realtime', checkedAt: now },
      url: `http://127.0.0.1:${port}/`,
    }
    const services = [gateway]
    for (const [index, device] of devices.entries()) {
      services.push({
        ...service(`${device.id}-ble`, 'ble', '蓝牙语音', device.id),
        state: 'running', pid: 42002 + index, owned: true, startedAt,
        connected: true, voiceReady: true, bleState: 'audio-connected',
        device: { firmware: '0.7.1-dev', board: device.board, playing: false,
          battery: index === 0 ? { valid: true, percent: 78, charging: false } : null, seenAt: now },
        metrics: { fps: 10, kibPerSecond: 31.3, at: now },
      })
      services.push(service(`${device.id}-bridge`, 'bridge', 'Wi-Fi Bridge', device.id))
    }
    return { updatedAt: now, startedAt, devices, services, logs: logs.read(),
      setup: { bleBuilt: true, gatewayInstalled: true, hostConfigured: true } }
  },
}
const app = createManagerServer(manager, {
  publicDirectory: fileURLToPath(new URL('../host/public/', import.meta.url)), port,
})
const address = await app.start()
console.log(`JUFF read-only UI preview: http://127.0.0.1:${address.port}/`)
console.log('Synthetic data only. No device, cloud, or local configuration access.')
let stopping = false
const stop = async () => { if (stopping) return; stopping = true; await app.close() }
process.once('SIGINT', () => void stop())
process.once('SIGTERM', () => void stop())
