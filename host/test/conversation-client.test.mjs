import assert from 'node:assert/strict'
import { EventEmitter } from 'node:events'
import test from 'node:test'
import { ConversationClient } from '../src/conversation-client.mjs'
import { GatewayClient } from '../src/gateway-client.mjs'

function fixture(t, { stalled = false, ready = true, outputEnabled = true } = {}) {
  const clients = [], events = []
  class FakeGateway extends EventEmitter {
    constructor() { super(); clients.push(this); this.connected = false; this.sent = []; this.health = { backend: {} } }
    async connect() {
      if (stalled) await new Promise(resolve => { this.finish = resolve })
      this.connected = true
      if (ready) this.emit('event', { type: 'voice.ready' })
      return this.health
    }
    close() { this.closed = true; this.connected = false; this.emit('close', 1000, 'closed') }
    send(event) { this.sent.push(event); return true }
    sendAudio(pcm) { return this.send({ type: 'audio.append', pcm }) }
  }
  const gate = new ConversationClient({ outputEnabled }, { GatewayClientImpl: FakeGateway })
  gate.on('event', event => events.push(event))
  gate.on('error', error => { throw error })
  t.after(() => gate.close())
  const state = (generation, active, mode = 'wake') => gate.setConversation({ type: 'conversation.state', generation, active, mode })
  return { gate, clients, events, state }
}

test('standby closes the cloud connection and fences old audio; wake creates a fresh session', async t => {
  const { gate, clients, events, state } = fixture(t)
  await gate.connect()
  const original = clients[0]
  assert.equal(gate.sendAudio(Buffer.from([1, 2])), true)
  state(2, false)
  assert.equal(original.closed, true)
  assert.equal(gate.sendAudio(Buffer.from([3, 4])), false)
  const count = events.length
  original.emit('event', { type: 'audio.delta', responseId: 'old' })
  assert.equal(events.length, count)
  state(3, true)
  await gate.connect()
  assert.equal(clients.length, 2)
  assert.ok(events.some(event => event.type === 'voice.ready' && event.conversationGeneration === 3))
  state(3, true) // A retried acknowledgement must not restart a live reply.
  assert.equal(clients.length, 2)
  assert.equal(state(2, false), false)
  assert.equal(gate.connected, true)
})

test('sleep during a slow connection never restores or forwards the retired session', async t => {
  const { gate, clients, events, state } = fixture(t, { stalled: true })
  const pending = gate.connect()
  state(1, false)
  clients[0].finish()
  assert.equal(await pending, null)
  assert.equal(clients[0].closed, true)
  assert.ok(!events.some(event => event.type === 'voice.ready'))
  assert.ok(!events.some(event => event.type === 'audio.delta'))
  state(2, true)
  const wake = gate.connect()
  state(3, false)
  clients[1].finish()
  assert.equal(await wake, null)
  assert.equal(gate.connected, false)
  assert.ok(!events.some(event => event.type === 'voice.ready'))
})

test('continuous mode and legacy firmware keep streaming; devices are independent', async t => {
  const left = fixture(t), right = fixture(t)
  await Promise.all([left.gate.connect(), right.gate.connect()])
  assert.ok(left.events.some(event => event.type === 'voice.ready' && !('conversationGeneration' in event)))
  left.state(1, false)
  assert.equal(right.gate.connected, true)
  left.state(2, true, 'continuous')
  await left.gate.connect()
  assert.equal(left.gate.connected, true)
  assert.equal(left.state(2, false), false)
  assert.equal(left.state(3, false, 'continuous'), false)
  assert.equal(left.state(1.5, false), false)
  assert.ok(!left.events.some(event => event.type === 'audio.delta'))
  assert.ok(!right.events.some(event => event.type === 'audio.delta'))
})

test('wake acknowledges once after cloud voice readiness, using short local PCM', async t => {
  const { gate, clients, events, state } = fixture(t, { ready: false })
  state(1, false)
  state(2, true)
  await gate.connect()
  assert.ok(!events.some(event => event.type === 'audio.delta'), 'Opening a WebSocket is not voice readiness')
  clients[0].emit('event', { type: 'voice.ready' })
  const audio = events.filter(event => event.type === 'audio.delta')
  assert.ok(audio.length)
  const pcm = Buffer.concat(audio.map(event => Buffer.from(event.audio, 'base64')))
  assert.ok(pcm.length >= 12000 && pcm.length <= 72000, 'Greeting must be 0.25–1.5 seconds')
  assert.equal(pcm.length % 2, 0)
  assert.ok(pcm.some(byte => byte !== 0))
  assert.ok(audio.every(event => event.sampleRate === 24000 && event.conversationGeneration === 2
    && event.responseId === audio[0].responseId && Buffer.byteLength(event.audio, 'base64') <= 9600))
  assert.ok(events.findIndex(event => event.type === 'voice.ready')
    < events.findIndex(event => event.type === 'audio.delta'))
  assert.deepEqual(events.at(-1), { type: 'audio.done', responseId: audio[0].responseId,
    conversationGeneration: 2 })
  state(2, true)
  clients[0].emit('event', { type: 'voice.ready' })
  assert.equal(events.filter(event => event.type === 'audio.done').length, 1)
  state(3, false)
  state(4, true)
  await gate.connect()
  clients[1].emit('event', { type: 'voice.ready' })
  const completions = events.filter(event => event.type === 'audio.done')
  assert.equal(completions.length, 2)
  assert.notEqual(completions[0].responseId, completions[1].responseId)
})

test('local greeting lifecycle stays local; microphone and real response events still reach the gateway', async t => {
  const { gate, clients, events, state } = fixture(t)
  state(1, true)
  await gate.connect()
  const responseId = events.find(event => event.type === 'audio.delta').responseId
  for (const type of ['playback.started', 'playback.ended', 'playback.cancelled']) {
    assert.equal(gate.send({ type, responseId }), true)
  }
  assert.deepEqual(clients[0].sent, [])
  gate.send({ type: 'playback.started', responseId: 'cloud-response' })
  gate.sendAudio(Buffer.from([1, 2]))
  gate.send({ type: 'interrupt' })
  assert.deepEqual(clients[0].sent.map(event => event.type), ['playback.started', 'audio.append', 'interrupt'])
  state(2, true, 'continuous')
  await gate.connect()
  gate.send({ type: 'playback.ended', responseId })
  assert.deepEqual(clients[1].sent, [], 'A late local event must not enter the next cloud session')
})

test('cloud retry retains a pending greeting but does not repeat one already played', async t => {
  const { gate, clients, events, state } = fixture(t, { ready: false })
  state(1, true)
  await gate.connect()
  clients[0].close()
  await gate.connect()
  clients[0].emit('event', { type: 'voice.ready' })
  assert.ok(!events.some(event => event.type === 'audio.delta'), 'Retired cloud connection must stay silent')
  clients[1].emit('event', { type: 'voice.ready' })
  clients[1].close()
  await gate.connect()
  clients[2].emit('event', { type: 'voice.ready' })
  assert.equal(events.filter(event => event.type === 'audio.done').length, 1)
})

test('stop during wake, disabled output and an early real reply suppress the greeting', async t => {
  for (const type of ['interrupt', 'sleep']) {
    const { gate, clients, events, state } = fixture(t, { ready: false })
    state(1, true)
    await gate.connect()
    gate.send({ type })
    clients[0].emit('event', { type: 'voice.ready' })
    assert.ok(!events.some(event => event.type === 'audio.delta'))
  }
  const muted = fixture(t, { outputEnabled: false })
  muted.state(1, true)
  await muted.gate.connect()
  assert.ok(!muted.events.some(event => event.type === 'audio.delta'))
  const speaking = fixture(t, { ready: false })
  speaking.state(1, true)
  await speaking.gate.connect()
  speaking.clients[0].emit('event', { type: 'response.started', responseId: 'cloud-response' })
  speaking.clients[0].emit('event', { type: 'voice.ready' })
  assert.ok(!speaking.events.some(event => event.type === 'audio.delta'))
})

test('sleeping while the cue is being forwarded fences the remaining audio', async t => {
  const { gate, clients, events, state } = fixture(t, { ready: false })
  state(1, true)
  await gate.connect()
  gate.on('event', event => { if (event.type === 'audio.delta') state(2, false) })
  clients[0].emit('event', { type: 'voice.ready' })
  assert.equal(events.filter(event => event.type === 'audio.delta').length, 1)
  assert.equal(events.filter(event => event.type === 'audio.done').length, 0)
  assert.equal(events.at(-2).type, 'playback.clear')
})

test('generation wrap accepts the new conversation and rejects previous events', async t => {
  const { gate, state } = fixture(t)
  state(0xffffffff, false)
  state(1, true)
  await gate.connect()
  assert.equal(state(0xffffffff, false), false)
  assert.equal(gate.connected, true)
})

test('closing a GatewayClient during health lookup never creates a late socket', async () => {
  let finish, sockets = 0
  const client = new GatewayClient({ baseUrl: 'http://127.0.0.1', sessionId: 'fixture',
    fetchImpl: () => new Promise(resolve => { finish = resolve }),
    WebSocketImpl: class { constructor() { sockets++ } },
  })
  const pending = client.connect()
  client.close()
  finish({ ok: true, json: async () => ({ backend: {} }) })
  await assert.rejects(pending, /closed/)
  assert.equal(sockets, 0)
})
