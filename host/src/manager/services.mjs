import { spawn } from 'node:child_process'
import { existsSync } from 'node:fs'
import path from 'node:path'
import { setTimeout as delay } from 'node:timers/promises'
import { readConfig } from '../config.mjs'
import { LogStore, readLines } from './logs.mjs'
import { ManagerError } from './profiles.mjs'

const active = record => Boolean(record.child) || record.state === 'external'
const text = (value, max = 80) => typeof value === 'string' ? value.slice(0, max) : ''
const number = value => typeof value === 'number' && Number.isFinite(value) ? value : null

export class ServiceManager {
  constructor({ root, store, env = process.env, spawnImpl = spawn, fetchImpl = fetch,
    now = Date.now, startTimeout = 12000, stopTimeout = 5000, pollDelay = 150,
    signalImpl = (child, signal) => process.kill(-child.pid, signal), checkFiles = true }) {
    Object.assign(this, { root, store, env, spawnImpl, fetchImpl, now, startTimeout, stopTimeout, pollDelay, signalImpl, checkFiles })
    this.logs = new LogStore({ secrets: [env.JUFF_DEVICE_TOKEN, env.DASHSCOPE_API_KEY], now })
    this.records = new Map()
    this.operations = Promise.resolve()
    this.startedAt = now()
    this.closing = false
    this.refreshing = null
    this.syncRecords()
  }

  config() { return readConfig(this.env) }
  gatewayURL() {
    try {
      const url = new URL(this.env.QWEN_GATEWAY_URL || 'http://127.0.0.1:3101')
      return ['http:', 'https:'].includes(url.protocol) ? url.origin : null
    } catch { return null }
  }
  syncRecords() {
    const definitions = [{ id: 'gateway', kind: 'gateway', name: 'Qwen Gateway', deviceId: null }]
    for (const device of this.store.list()) {
      definitions.push({ id: `${device.id}-ble`, kind: 'ble', name: '蓝牙语音', deviceId: device.id })
      definitions.push({ id: `${device.id}-bridge`, kind: 'bridge', name: 'Wi-Fi Bridge', deviceId: device.id })
    }
    for (const item of definitions) {
      if (!this.records.has(item.id)) this.records.set(item.id, {
        ...item, state: 'stopped', child: null, startedAt: null, stoppedAt: null,
        lastError: null, connected: false, voiceReady: false, device: null, metrics: null, health: null,
      })
    }
    for (const [id] of this.records) if (!definitions.some(item => item.id === id)) this.records.delete(id)
  }
  record(id) {
    const record = this.records.get(id)
    if (!record) throw new ManagerError('没有找到这个服务', 404)
    return record
  }
  profile(id) {
    const profile = this.store.list().find(item => item.id === id)
    if (!profile) throw new ManagerError('没有找到这台设备', 404)
    return profile
  }
  exclusive(operation) {
    if (this.closing) return Promise.reject(new ManagerError('管理后台正在关闭', 503))
    const result = this.operations.then(operation)
    this.operations = result.catch(() => {})
    return result
  }

  async probe(url, gateway = false) {
    if (!url) return null
    try {
      const response = await this.fetchImpl(url, { signal: AbortSignal.timeout(1000), redirect: 'error' })
      if (!response.ok) return null
      const data = await response.json()
      if (gateway ? !data?.backend : data?.ok !== true) return null
      return gateway ? { ok: true, model: text(data.realtimeModel || data.realtimeModelProfile?.id), checkedAt: this.now() }
        : { ok: true, deviceConnected: data.deviceConnected === true, checkedAt: this.now() }
    } catch { return null }
  }
  healthURL(record) {
    if (record.kind === 'gateway') return this.gatewayURL() ? `${this.gatewayURL()}/api/health` : null
    const host = this.env.JUFF_BRIDGE_HOST || '127.0.0.1'
    const urlHost = ['0.0.0.0', '::', 'localhost'].includes(host) ? '127.0.0.1' : host
    return `http://${urlHost.includes(':') ? `[${urlHost}]` : urlHost}:${this.profile(record.deviceId).bridgePort}/healthz`
  }
  async refresh() {
    if (this.refreshing) return this.refreshing
    this.refreshing = (async () => {
      await Promise.all([...this.records.values()].filter(record => record.kind !== 'ble').map(async record => {
        const child = record.child
        const health = await this.probe(this.healthURL(record), record.kind === 'gateway')
        if (record.child !== child) return // A result from before restart cannot mark the replacement ready.
        record.health = health
        if (record.kind === 'bridge') record.connected = health?.deviceConnected === true
        if (!child && !['starting', 'stopping'].includes(record.state)) {
          if (health) record.state = 'external'
          else if (record.state === 'external') record.state = 'stopped'
        }
      }))
    })().finally(() => { this.refreshing = null })
    return this.refreshing
  }
  snapshot() {
    const devices = this.store.list()
    const services = [...this.records.values()].map(record => ({
      id: record.id, kind: record.kind, deviceId: record.deviceId, name: record.name,
      state: record.state === 'running' && record.kind !== 'ble' && !record.health ? 'unhealthy' : record.state,
      pid: record.child?.pid || null, owned: Boolean(record.child), startedAt: record.startedAt,
      lastError: record.lastError, connected: record.connected, voiceReady: record.voiceReady,
      device: record.device, metrics: record.metrics, health: record.health,
      bleState: record.bleState || null,
      url: record.kind === 'gateway' ? this.gatewayURL() : record.kind === 'bridge' ? this.healthURL(record) : null,
    }))
    return {
      updatedAt: this.now(), startedAt: this.startedAt, devices, services,
      setup: {
        bleBuilt: existsSync(path.join(this.root, 'macos/build/JuffBLE.app/Contents/MacOS/JuffBLE')),
        gatewayInstalled: existsSync(path.join(this.root, 'host/node_modules/.bin/qwenaudio')),
        hostConfigured: (() => { try { this.config(); return true } catch { return false } })(),
      },
      logs: this.logs.read().slice(-80),
    }
  }

  handleLine(record, line, level = 'info') {
    if (line.startsWith('Juff ready:')) record.state = 'running'
    if (line.startsWith('BLE STATE ')) {
      record.bleState = line.slice(10).split(':')[0]
      if (record.bleState === 'audio-connected') record.connected = true
      if (['disconnected', 'scanning', 'powered-off', 'resetting', 'unsupported', 'unauthorized'].includes(record.bleState)) {
        record.connected = false; record.voiceReady = false; record.metrics = null
      }
      if (record.bleState === 'sent') return
    }
    if (line.startsWith('BLE voice ready:')) record.voiceReady = true
    if (line.includes('BLE Qwen session closed')) record.voiceReady = false
    const metrics = /^BLE MIC ([\d.]+) fps; ([\d.]+) KiB\/s; peak RMS (\d+)/.exec(line)
    if (metrics) {
      record.metrics = { ...record.metrics, fps: Number(metrics[1]), kibPerSecond: Number(metrics[2]), rms: Number(metrics[3]), at: this.now() }
      return
    }
    if (line.startsWith('BLE DEVICE ')) {
      try {
        const event = JSON.parse(line.slice(11))
        if (event.type === 'status') {
          record.device = {
            firmware: text(event.firmware), board: text(event.board), playing: event.playing === true,
            voiceInterrupt: event.voiceInterrupt === true, speakerFlow: event.speakerFlow === 1,
            battery: event.battery ? { valid: event.battery.valid === true, percent: number(event.battery.percent), charging: event.battery.charging === true } : null,
            seenAt: this.now(),
          }
          record.connected = event.bleAudio === true
          record.voiceReady = event.voice === true
          return
        }
        if (event.type === 'speaker.credit') {
          record.metrics = { ...record.metrics, queuedMs: number(event.queuedMs), underruns: number(event.underruns), dropped: number(event.dropped) }
          return
        }
        if (event.type === 'ack' && event.request === 'status') return
      } catch { /* Preserve malformed operational output as a bounded log line. */ }
    }
    if (/startup failed:|Unable to start|websocket error|notify-error|connect-error/.test(line)) {
      record.lastError = this.logs.redact(line); level = 'error'
    }
    this.logs.add(record.id, line, level)
  }

  async waitUntil(check, timeout, description) {
    const until = this.now() + timeout
    do {
      if (await check()) return
      await delay(this.pollDelay)
    } while (this.now() < until)
    throw new ManagerError(description, 504)
  }
  launch(record, executable, args, env) {
    record.state = 'starting'; record.lastError = null; record.health = null
    record.connected = false; record.voiceReady = false; record.device = null; record.metrics = null; record.bleState = null
    record.startedAt = this.now()
    const child = this.spawnImpl(executable, args, {
      cwd: path.join(this.root, 'host'), env, stdio: ['ignore', 'pipe', 'pipe'], detached: true,
    })
    record.child = child
    readLines(child.stdout, line => { if (record.child === child) this.handleLine(record, line) })
    readLines(child.stderr, line => { if (record.child === child) this.handleLine(record, line, 'info') })
    child.once('error', error => {
      record.lastError = this.logs.redact(error.message)
      this.logs.add(record.id, record.lastError, 'error')
      record.state = 'error'
      if (!child.pid && record.child === child) record.child = null
    })
    child.once('exit', (code, signal) => {
      const expected = record.state === 'stopping'
      // A worker can own a BLE child. Clean its process group even if the worker crashed.
      try { this.signalImpl(child, 'SIGTERM') } catch { /* Process group may already be gone. */ }
      if (record.child !== child) return
      record.child = null; record.health = null; record.connected = false; record.voiceReady = false; record.metrics = null
      record.state = expected ? 'stopped' : 'error'; record.stoppedAt = this.now()
      if (!expected) record.lastError ||= `服务意外退出（${signal || code}）`
      this.logs.add(record.id, expected ? '服务已停止' : record.lastError, expected ? 'info' : 'error')
    })
    this.logs.add(record.id, '正在启动服务')
  }
  async start(record) {
    await this.refresh()
    if (record.child && record.state !== 'error') return
    if (record.state === 'external') return
    if (record.child) await this.stop(record)
    const config = this.config()
    let dependents = []
    if (record.kind !== 'gateway') {
      const device = this.profile(record.deviceId)
      if (record.kind === 'ble' && !device.bleName) throw new ManagerError('请先在设备设置中填写蓝牙名称')
      const executable = path.join(this.root, 'macos/build/JuffBLE.app/Contents/MacOS/JuffBLE')
      if (record.kind === 'ble' && this.checkFiles && !existsSync(executable)) throw new ManagerError('请先运行 make ble-build 编译蓝牙程序', 409)
      await this.start(this.record('gateway'))
      this.launch(record, process.execPath, ['--env-file=.env', 'src/run.mjs'], {
        ...this.env, JUFF_SERVICE_MODE: record.kind, JUFF_BLE_DEVICE: device.bleName,
        JUFF_BRIDGE_PORT: String(device.bridgePort), QWEN_SESSION_ID: device.sessionId,
      })
    } else {
      const url = new URL(config.gatewayUrl)
      if (url.protocol !== 'http:' || !['localhost', '127.0.0.1'].includes(url.hostname)) throw new ManagerError('此网关地址需要在外部启动；后台只能启动本机 HTTP 网关', 409)
      const executable = path.join(this.root, 'host/node_modules/.bin/qwenaudio')
      if (this.checkFiles && !existsSync(executable)) throw new ManagerError('Qwen 依赖未安装，请先运行 make setup', 409)
      // A crashed gateway leaves workers alive with closed voice sessions.
      // Recover them for every startup entry point, including "start all".
      dependents = [...this.records.values()].filter(item => item.kind !== 'gateway' && item.child)
      for (const dependent of dependents) await this.stop(dependent)
      this.launch(record, executable, [], { ...this.env, PORT: url.port || '80' })
    }
    try {
      await this.waitUntil(async () => {
        if (record.state === 'error' || !record.child) throw new ManagerError(record.lastError || '服务启动失败', 502)
        if (record.kind === 'ble') return record.state === 'running'
        record.health = await this.probe(this.healthURL(record), record.kind === 'gateway')
        if (!record.health) return false
        record.state = 'running'; return true
      }, this.startTimeout, '服务启动超时，请查看运行日志')
      this.logs.add(record.id, '服务已就绪')
    } catch (error) {
      await this.stop(record)
      record.state = 'error'; record.lastError = this.logs.redact(error.message)
      this.logs.add(record.id, record.lastError, 'error')
      throw error
    }
    for (const dependent of dependents) await this.start(dependent)
  }
  async stop(record) {
    const child = record.child
    if (!child) return
    record.state = 'stopping'
    try { this.signalImpl(child, 'SIGTERM') } catch (error) { if (error.code !== 'ESRCH') throw error }
    try {
      await this.waitUntil(() => record.child !== child, this.stopTimeout, '停止超时')
    } catch {
      try { this.signalImpl(child, 'SIGKILL') } catch (error) { if (error.code !== 'ESRCH') throw error }
      await this.waitUntil(() => record.child !== child, this.stopTimeout, '服务未退出，请查看日志')
    }
  }
  async stopOwned() {
    for (const record of this.records.values()) if (record.kind !== 'gateway') await this.stop(record)
    await this.stop(this.record('gateway'))
  }
  command(id, action) {
    return this.exclusive(async () => {
      if (!['start', 'stop', 'restart'].includes(action)) throw new ManagerError('不支持的服务操作')
      await this.refresh()
      if (id === 'all') {
        if (action !== 'start') await this.stopOwned()
        if (action !== 'stop') {
          await this.start(this.record('gateway'))
          for (const device of this.store.list()) if (device.bleName) await this.start(this.record(`${device.id}-ble`))
        }
      } else {
        const record = this.record(id)
        if (record.state === 'external' && action !== 'start') throw new ManagerError('服务由外部进程启动，请在原终端停止它', 409)
        const dependents = record.kind === 'gateway' && action !== 'start'
          ? [...this.records.values()].filter(item => item.kind !== 'gateway' && item.child) : []
        if (action !== 'start') {
          for (const dependent of dependents) await this.stop(dependent)
          await this.stop(record)
        }
        if (action !== 'stop') {
          await this.start(record)
          for (const dependent of dependents) await this.start(dependent)
        }
      }
      return this.snapshot()
    })
  }
  updateProfiles(operation) {
    return this.exclusive(async () => {
      await this.refresh()
      const before = this.store.list()
      const after = operation(before.map(item => ({ ...item })))
      for (const profile of before) {
        const changed = JSON.stringify(profile) !== JSON.stringify(after.find(item => item.id === profile.id))
        if (changed && ['ble', 'bridge'].some(kind => active(this.record(`${profile.id}-${kind}`)))) {
          throw new ManagerError('请先停止这台设备的服务，再修改或移除配置', 409)
        }
      }
      await this.store.save(after)
      this.syncRecords()
      this.logs.add('manager', '设备配置已保存')
      return this.snapshot()
    })
  }
  async close() {
    this.closing = true
    await this.operations
    await this.stopOwned()
  }
}
