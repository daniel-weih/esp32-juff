import assert from 'node:assert/strict'
import { EventEmitter } from 'node:events'
import { PassThrough } from 'node:stream'
import { mkdtemp, readFile, rm, stat } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import test from 'node:test'
import { ProfileStore, validateProfiles } from '../src/manager/profiles.mjs'
import { ServiceManager } from '../src/manager/services.mjs'
import { LogStore, readLines } from '../src/manager/logs.mjs'
import { AssistantProfileStore } from '../src/manager/assistant.mjs'

async function fixture(t, options = {}) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'juff-manager-'))
  t.after(() => rm(root, { recursive: true, force: true }))
  const store = new ProfileStore(path.join(root, 'devices.json'), [3210, 3101])
  await store.load()
  await store.save(store.list().map((device, index) => ({ ...device, bleName: `JUFF-TEST${index}` })))
  const children = [], events = [], external = new Map()
  let pid = 10000
  const spawnImpl = (executable, args, config) => {
    const child = new EventEmitter()
    Object.assign(child, { pid: ++pid, stdout: new PassThrough(), stderr: new PassThrough(), exitCode: null, config })
    child.kind = executable.endsWith('qwenaudio') ? 'gateway' : config.env.JUFF_SERVICE_MODE
    child.end = (code = 0) => { if (child.exitCode !== null) return; child.exitCode = code; child.emit('exit', code, null) }
    children.push(child); events.push(`start:${child.kind}:${config.env.QWEN_SESSION_ID || ''}`)
    queueMicrotask(() => {
      if (options.spawnError) { child.pid = undefined; child.emit('error', new Error('spawn denied')); return }
      if (!options.timeout) child.stdout.write('Juff ready: fixture\n')
    })
    return child
  }
  const fetchImpl = async url => {
    const parsed = new URL(url)
    const gateway = parsed.pathname === '/api/health'
    const owned = children.find(child => child.exitCode === null && child.kind === (gateway ? 'gateway' : 'bridge')
      && (gateway || child.config.env.JUFF_BRIDGE_PORT === parsed.port))
    const live = external.get(parsed.port) || (owned && !options.timeout && !options.spawnError)
    return { ok: Boolean(live), json: async () => gateway ? { backend: {}, realtimeModel: 'test-model' } : { ok: true, deviceConnected: false } }
  }
  const signalImpl = (child, signal) => {
    if (child.exitCode !== null) return
    events.push(`${signal}:${child.kind}`)
    if (options.ignoreTerm && signal === 'SIGTERM') return
    child.end()
  }
  const manager = new ServiceManager({ root, store, env: { JUFF_DEVICE_TOKEN: 'test-device-token-12345', QWEN_GATEWAY_URL: 'http://127.0.0.1:3101' },
    assistantStore: new AssistantProfileStore({ file: path.join(root, 'ASSISTANT.md') }),
    spawnImpl, fetchImpl, signalImpl, checkFiles: false, startTimeout: 30, stopTimeout: 20, pollDelay: 1 })
  t.after(() => manager.close())
  return { manager, store, children, events, external }
}

test('profiles persist privately, preserve both boards, and reject duplicate ports and bindings', async t => {
  const { store } = await fixture(t)
  const value = JSON.parse(await readFile(store.file, 'utf8'))
  assert.equal(value.devices.length, 2)
  assert.equal((await stat(store.file)).mode & 0o777, 0o600)
  const list = store.list()
  assert.throws(() => validateProfiles(list.map(item => ({ ...item, bridgePort: 8766 }))), /端口/)
  assert.throws(() => validateProfiles(list.map(item => ({ ...item, bleName: ' JUFF-SAME ' }))), /重复绑定/)
  assert.throws(() => validateProfiles([{ ...list[0], bridgePort: 3210 }], [3210]), /端口/)
  assert.throws(() => validateProfiles([{ ...list[0], board: '__proto__' }]), /硬件/)
  await assert.rejects(store.save([{ ...list[0], id: '../bad' }]), /编号/)
  assert.deepEqual(store.list(), list)
})

test('concurrent device starts launch one shared gateway and isolated BLE workers', async t => {
  const { manager, children } = await fixture(t)
  await Promise.all([manager.command('small-ble', 'start'), manager.command('large-ble', 'start'), manager.command('small-ble', 'start')])
  assert.deepEqual(children.map(child => child.kind), ['gateway', 'ble', 'ble'])
  assert.match((await manager.assistantStore.read()).content, /默认昵称叫“喵喵”/)
  assert.equal(children[1].config.env.JUFF_BLE_DEVICE, 'JUFF-TEST0')
  assert.equal(children[2].config.env.JUFF_BLE_DEVICE, 'JUFF-TEST1')
  assert.notEqual(children[1].config.env.QWEN_SESSION_ID, children[2].config.env.QWEN_SESSION_ID)
  assert.equal(children[1].config.env.JUFF_SERVICE_MODE, 'ble')
  assert.equal(children[1].config.detached, true)
  assert.ok(!JSON.stringify(manager.snapshot()).includes('test-device-token-12345'))
  await manager.command('small-ble', 'stop')
  assert.equal(manager.record('small-ble').state, 'stopped')
  assert.equal(manager.record('large-ble').state, 'running')
  assert.equal(manager.record('gateway').state, 'running')
})

test('saving assistant settings does not interrupt services; apply recreates only previously running connections', async t => {
  const { manager, store, children } = await fixture(t)
  manager.assistantStore = new AssistantProfileStore({ file: path.join(path.dirname(store.file), 'ASSISTANT.md') })
  await manager.command('large-ble', 'start')
  const oldGateway = manager.record('gateway').child, oldVoice = manager.record('large-ble').child
  let profile = await manager.readAssistant()
  profile = await manager.saveAssistant({ content: '你叫小智。', revision: profile.revision })
  assert.equal(manager.record('large-ble').child, oldVoice)
  assert.equal(profile.reconnected, 0)
  profile = await manager.saveAssistant({ content: '你叫小智，回答简短。', revision: profile.revision, reconnect: true })
  assert.equal(profile.reconnected, 1)
  assert.equal(profile.applyError, null)
  assert.equal(manager.record('gateway').child, oldGateway)
  assert.notEqual(manager.record('large-ble').child, oldVoice)
  assert.equal(manager.record('small-ble').state, 'stopped')
  assert.deepEqual(children.map(item => item.kind), ['gateway', 'ble', 'ble'])
  assert.ok(!JSON.stringify(manager.snapshot()).includes('你叫小智'))
})

test('assistant edits serialize stale revisions and reject reconnecting unmanaged gateways', async t => {
  const { manager, store, external } = await fixture(t)
  manager.assistantStore = new AssistantProfileStore({ file: path.join(path.dirname(store.file), 'ASSISTANT.md') })
  const profile = await manager.readAssistant()
  const results = await Promise.allSettled(['一', '二'].map(content => manager.saveAssistant({ content, revision: profile.revision })))
  assert.deepEqual(results.map(item => item.status), ['fulfilled', 'rejected'])
  external.set('3101', true)
  await manager.refresh()
  const current = await manager.readAssistant()
  await assert.rejects(manager.saveAssistant({ content: '外部', revision: current.revision, reconnect: true }), /外部管理/)
  assert.equal((await manager.readAssistant()).content, '一')
  manager.env.QWEN_GATEWAY_URL = 'https://gateway.example.invalid'
  await assert.rejects(manager.readAssistant(), /外部网关/)
})

test('failed assistant reconnect reports that the profile was saved instead of claiming it applied', async t => {
  const { manager, store } = await fixture(t)
  manager.assistantStore = new AssistantProfileStore({ file: path.join(path.dirname(store.file), 'ASSISTANT.md') })
  await manager.command('large-ble', 'start')
  const profile = await manager.readAssistant()
  await manager.command('large-bridge', 'start')
  const start = manager.start.bind(manager)
  manager.start = async record => {
    if (record.kind === 'ble') throw new Error('simulated reconnect failure')
    return start(record)
  }
  const result = await manager.saveAssistant({ content: '已保存的设定', revision: profile.revision, reconnect: true })
  assert.match(result.applyError, /设定已保存/)
  assert.equal(result.reconnected, 1)
  assert.equal(manager.record('large-bridge').state, 'running')
  assert.equal((await manager.readAssistant()).content, '已保存的设定')
})

test('gateway restart preserves only active dependents; stop orders children before gateway', async t => {
  const { manager, events } = await fixture(t)
  await manager.command('small-ble', 'start')
  await manager.command('large-bridge', 'start')
  await manager.command('gateway', 'restart')
  assert.equal(manager.record('large-ble').state, 'stopped')
  assert.equal(manager.record('large-bridge').state, 'running')
  const firstStop = events.indexOf('SIGTERM:ble')
  const gatewayStop = events.indexOf('SIGTERM:gateway')
  assert.ok(firstStop < gatewayStop && events.indexOf('SIGTERM:bridge') < gatewayStop)
  await manager.command('gateway', 'stop')
  assert.ok([...manager.records.values()].every(record => !record.child))
})

test('external gateway is reused and never signalled by stop all', async t => {
  const { manager, external, children, events } = await fixture(t)
  external.set('3101', true)
  await manager.command('small-ble', 'start')
  assert.equal(manager.record('gateway').state, 'external')
  assert.deepEqual(children.map(child => child.kind), ['ble'])
  await assert.rejects(manager.command('gateway', 'restart'), /外部进程/)
  await manager.command('all', 'stop')
  assert.equal(manager.record('gateway').state, 'external')
  assert.ok(!events.includes('SIGTERM:gateway'))
})

test('starting a crashed gateway reconnects its surviving voice workers', async t => {
  const { manager, children } = await fixture(t)
  await manager.command('small-ble', 'start')
  children[0].end(1)
  assert.equal(manager.record('gateway').state, 'error')
  const oldWorker = manager.record('small-ble').child
  await manager.command('gateway', 'start')
  assert.notEqual(manager.record('small-ble').child, oldWorker)
  assert.equal(manager.record('gateway').state, 'running')
})

test('start all recovers both BLE and Bridge workers after a gateway crash', async t => {
  const { manager, children } = await fixture(t)
  await manager.command('small-ble', 'start')
  await manager.command('large-bridge', 'start')
  const oldBle = manager.record('small-ble').child
  const oldBridge = manager.record('large-bridge').child
  children[0].end(1)
  await manager.command('all', 'start')
  assert.notEqual(manager.record('small-ble').child, oldBle)
  assert.notEqual(manager.record('large-bridge').child, oldBridge)
  assert.equal(oldBle.exitCode, 0)
  assert.equal(oldBridge.exitCode, 0)
  for (const id of ['gateway', 'small-ble', 'large-ble', 'large-bridge']) {
    assert.equal(manager.record(id).state, 'running')
  }
  assert.equal(manager.record('small-bridge').state, 'stopped')
})

test('timeouts and spawn errors leave no owned worker; operations recover after failure', async t => {
  for (const settings of [{ timeout: true }, { spawnError: true }]) {
    const { manager } = await fixture(t, settings)
    await assert.rejects(manager.command('gateway', 'start'))
    assert.equal(manager.record('gateway').child, null)
    assert.equal(manager.record('gateway').state, 'error')
    await manager.command('all', 'stop')
  }
})

test('a worker ignoring SIGTERM is killed only within its owned process group', async t => {
  const { manager, events } = await fixture(t, { ignoreTerm: true })
  await manager.command('small-ble', 'start')
  await manager.command('small-ble', 'stop')
  assert.ok(events.includes('SIGKILL:ble'))
  assert.ok(!events.includes('SIGKILL:gateway'))
})

test('running profile cannot be edited or deleted; stopped profile changes persist', async t => {
  const { manager, store } = await fixture(t)
  await manager.command('small-ble', 'start')
  await assert.rejects(manager.updateProfiles(profiles => profiles.filter(item => item.id !== 'small')), /先停止/)
  await assert.rejects(manager.updateProfiles(profiles => profiles.map(item => ({ ...item, name: 'changed' }))), /先停止/)
  await manager.command('small-ble', 'stop')
  await manager.updateProfiles(profiles => profiles.map(item => item.id === 'small' ? { ...item, name: '小伙伴' } : item))
  assert.equal(store.list()[0].name, '小伙伴')
})

test('unbound BLE refuses to start before creating a cloud gateway', async t => {
  const { manager, children } = await fixture(t)
  await manager.updateProfiles(profiles => profiles.map(item => ({ ...item, bleName: '' })))
  await assert.rejects(manager.command('small-ble', 'start'), /蓝牙名称/)
  assert.equal(children.length, 0)
})

test('actual BLE output updates battery and metrics, disconnect clears live state, late output is ignored', async t => {
  const { manager, children } = await fixture(t)
  await manager.command('small-ble', 'start')
  const old = children[1]
  old.stdout.write('BLE DEVICE ' + JSON.stringify({ type: 'status', board: 'waveshare-lcd-1.54', firmware: '0.6.10-dev', bleAudio: true, voice: true, battery: { valid: true, percent: 78, charging: true }, token: 'untrusted' }) + '\n')
  old.stdout.write('BLE MIC 9.8 fps; 30.6 KiB/s; peak RMS 200\n')
  const record = manager.record('small-ble')
  assert.deepEqual(record.device.battery, { valid: true, percent: 78, charging: true })
  assert.equal(record.metrics.fps, 9.8)
  assert.equal(record.voiceReady, true)
  assert.ok(!JSON.stringify(manager.snapshot()).includes('untrusted'))
  old.stdout.write('BLE STATE disconnected:fixture\n')
  assert.equal(record.connected, false)
  assert.equal(record.metrics, null)
  await manager.command('small-ble', 'restart')
  old.stdout.write('BLE STATE audio-connected:obsolete\n')
  assert.equal(record.connected, false)
})

test('logs are bounded and redact credentials while omitting speech and audio payloads', () => {
  const secret = 'private-value-' + 'x'.repeat(20)
  const logs = new LogStore({ secrets: [secret], limit: 3 })
  logs.add('ble', 'QWEN user: private speech')
  logs.add('ble', 'MIC YWJjZGVm')
  assert.equal(logs.read().length, 0)
  for (const message of [`token=${secret}`, '{"apiKey":"unknown-sensitive"}', 'Authorization: Bearer another-secret-value']) logs.add('gateway', message)
  const serialized = JSON.stringify(logs.read())
  for (const value of [secret, 'unknown-sensitive', 'another-secret-value']) assert.ok(!serialized.includes(value))
  logs.add('manager', 'ready')
  assert.equal(logs.read().length, 3)
  assert.equal(logs.read({ service: 'manager' }).length, 1)
  const stream = new PassThrough(), lines = []
  readLines(stream, line => lines.push(line))
  stream.write('a'); stream.write('b\nsecond\n' + 'x'.repeat(20000))
  assert.deepEqual(lines.slice(0, 2), ['ab', 'second'])
  assert.ok(lines[2].length < 1300)
})
