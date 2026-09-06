import assert from 'node:assert/strict'
import { once } from 'node:events'
import http from 'node:http'
import { createRequire } from 'node:module'
import path from 'node:path'
import { setTimeout as delay } from 'node:timers/promises'
import { pathToFileURL } from 'node:url'
import test from 'node:test'
import WebSocket, { WebSocketServer } from 'ws'
import { ConversationClient } from '../src/conversation-client.mjs'

const require = createRequire(import.meta.url)
const packageRoot = path.dirname(require.resolve('qwen-audio-agent/package.json'))
const { ActiveVoiceClients } = await import(pathToFileURL(path.join(packageRoot,
  'server/src/voice/active-voice-clients.mjs')))

async function until(predicate) {
  const deadline = Date.now() + 1800
  while (!predicate()) {
    assert.ok(Date.now() < deadline, 'Expected ownership/voice event did not arrive')
    await delay(5)
  }
}

async function fixture(t, { duplicateAvailability = false } = {}) {
  // Use real WebSockets, production clients and the pinned Qwen arbitration.
  // Only health/auth and the cloud audio frontend are local synthetic fixtures.
  const arbitration = new ActiveVoiceClients(), peers = [], devices = []
  const server = http.createServer((request, response) => {
    response.writeHead(200, { 'content-type': 'application/json' })
    response.end(JSON.stringify({ backend: { protocol: 'none' } }))
  })
  const wss = new WebSocketServer({ server })
  const send = (peer, event) => {
    if (peer.ws.readyState === WebSocket.OPEN) peer.ws.send(JSON.stringify(event))
  }
  const broadcast = () => {
    const active = arbitration.active('fixture-owner')
    for (const peer of peers) {
      const state = active === peer ? 'active' : active ? 'busy' : 'available'
      for (let i = 0; i < (duplicateAvailability && state === 'available' ? 3 : 1); i++) {
        send(peer, { type: 'voice.ownership', state, holder: active ? { type: 'cli' } : null })
      }
    }
  }
  wss.on('connection', (ws, request) => {
    const peer = { ws, id: new URL(request.url, 'http://localhost').searchParams.get('sessionId'), requests: [],
      isAlive: () => ws.readyState === WebSocket.OPEN,
      deactivate: () => {
        send(peer, { type: 'playback.clear' })
        send(peer, { type: 'voice.deactivated' })
      },
    }
    peers.push(peer)
    // A socket can see availability before its first CONNECT is processed.
    broadcast()
    ws.on('message', data => {
      const event = JSON.parse(data)
      peer.requests.push(event)
      if (event.type === 'connect' || event.type === 'unmute') {
        const { granted } = arbitration.activate('fixture-owner', peer, { takeover: event.takeover })
        broadcast()
        if (granted) queueMicrotask(() => {
          if (arbitration.isActive('fixture-owner', peer)) send(peer, { type: 'voice.ready' })
        })
      } else if (event.type === 'mute') {
        arbitration.release('fixture-owner', peer)
        broadcast()
      }
    })
    ws.on('close', () => { arbitration.release('fixture-owner', peer); broadcast() })
  })
  server.listen(0, '127.0.0.1')
  await once(server, 'listening')
  t.after(async () => {
    for (const device of devices) device.gate.close()
    for (const peer of peers) peer.ws.terminate()
    await new Promise(resolve => wss.close(resolve))
    await new Promise(resolve => server.close(resolve))
  })
  return {
    arbitration, peers,
    async device(id, { mode = 'continuous', takeover = false, ...options } = {}) {
      const events = []
      const gate = new ConversationClient({ baseUrl: `http://127.0.0.1:${server.address().port}`,
        sessionId: id, takeover, ...options })
      const device = { gate, events, ready: () => events.filter(e => e.type === 'voice.ready').length,
        claims: () => peers.filter(p => p.id === id).flatMap(p => p.requests.filter(e => e.type === 'connect')) }
      devices.push(device)
      gate.on('event', event => events.push(event))
      gate.on('error', error => assert.fail(error.message))
      gate.setConversation({ type: 'conversation.state', mode, active: true, generation: 1 })
      await gate.connect()
      await until(() => device.claims().length > 0 && events.some(e => e.type === 'voice.ownership'))
      return device
    },
  }
}

test('a busy device acquires the released voice slot without reconnecting or changing capabilities', async t => {
  const f = await fixture(t)
  const first = await f.device('first'), waiting = await f.device('waiting', { inputEnabled: false })
  await until(() => first.ready() === 1 && waiting.events.some(e => e.state === 'busy'))
  assert.equal(waiting.ready(), 0)
  assert.equal(waiting.gate.connected, true)
  first.gate.close()
  await until(() => waiting.ready() === 1)
  assert.equal(f.peers.filter(p => p.id === 'waiting').length, 1)
  assert.equal(waiting.claims().length, 2)
  assert.deepEqual(waiting.claims()[1], { ...waiting.claims()[0], takeover: false })
  assert.equal(waiting.claims()[1].inputEnabled, false)
})

test('an evicted wake conversation resumes without taking over again or repeating its greeting', async t => {
  const f = await fixture(t)
  const first = await f.device('first', { mode: 'wake', takeover: true })
  await until(() => first.ready() === 1)
  const replacement = await f.device('replacement', { takeover: true })
  await until(() => replacement.ready() === 1 && first.events.some(e => e.type === 'voice.deactivated'))
  replacement.gate.close()
  await until(() => first.ready() === 2)
  assert.equal(first.claims().length, 2)
  assert.equal(first.claims()[0].takeover, true)
  assert.equal(first.claims()[1].takeover, false)
  assert.equal(first.events.filter(e => e.type === 'audio.done').length, 1)
})

test('duplicate availability and competing waiters never cause a takeover loop', async t => {
  const f = await fixture(t, { duplicateAvailability: true })
  const holder = await f.device('holder'), left = await f.device('left'), right = await f.device('right')
  await until(() => holder.ready() === 1 && left.events.some(e => e.state === 'busy')
    && right.events.some(e => e.state === 'busy'))
  holder.gate.close()
  await until(() => left.ready() + right.ready() === 1)
  await delay(40)
  assert.equal(left.claims().length, 2)
  assert.equal(right.claims().length, 2)
  const winner = left.ready() ? left : right, waiting = left.ready() ? right : left
  winner.gate.close()
  await until(() => waiting.ready() === 1)
  assert.equal(waiting.claims().length, 3)
})

test('wake timeout while waiting retires the old socket and requires a fresh wake', async t => {
  const f = await fixture(t)
  const holder = await f.device('holder'), waiting = await f.device('waiting', { mode: 'wake' })
  await until(() => waiting.events.some(e => e.state === 'busy'))
  const oldSocket = waiting.gate.client.socket
  waiting.gate.setConversation({ type: 'conversation.state', mode: 'wake', active: false, generation: 2 })
  holder.gate.close()
  await until(() => f.arbitration.size === 0)
  oldSocket.emit('message', Buffer.from('{"type":"voice.ownership","state":"available"}'))
  assert.equal(waiting.gate.connected, false)
  assert.equal(waiting.ready(), 0)
  assert.equal(waiting.claims().length, 1)
  waiting.gate.setConversation({ type: 'conversation.state', mode: 'wake', active: true, generation: 3 })
  await until(() => waiting.ready() === 1)
  assert.equal(waiting.events.find(e => e.type === 'voice.ready').conversationGeneration, 3)
})

test('explicit mute or sleep cancels automatic recovery until the user resumes', async t => {
  for (const type of ['mute', 'sleep']) {
    const f = await fixture(t)
    const holder = await f.device('holder'), waiting = await f.device('waiting')
    await until(() => waiting.events.some(e => e.state === 'busy'))
    waiting.gate.send({ type })
    await until(() => f.peers.find(p => p.id === 'waiting').requests.some(e => e.type === type))
    holder.gate.close()
    await until(() => waiting.events.some(e => e.state === 'available'))
    await delay(40)
    assert.equal(waiting.ready(), 0)
    assert.equal(waiting.claims().length, 1)
    waiting.gate.send({ type: 'unmute' })
    await until(() => waiting.ready() === 1)
  }
})

test('microphone mute survives reacquiring an output-capable voice slot', async t => {
  const f = await fixture(t)
  const holder = await f.device('holder'), waiting = await f.device('waiting')
  await until(() => waiting.events.some(e => e.state === 'busy'))
  waiting.gate.send({ type: 'input.mute' })
  await until(() => f.peers.find(p => p.id === 'waiting').requests.some(e => e.type === 'input.mute'))
  holder.gate.close()
  await until(() => waiting.ready() === 1)
  assert.equal(waiting.claims()[1].inputEnabled, false)
  assert.equal(waiting.claims()[1].outputEnabled, true)
})

test('stopping from an availability callback cannot create another voice request', async t => {
  const f = await fixture(t)
  const holder = await f.device('holder'), waiting = await f.device('waiting')
  await until(() => waiting.events.some(e => e.state === 'busy'))
  waiting.gate.on('event', event => {
    if (event.type === 'voice.ownership' && event.state === 'available') waiting.gate.close()
  })
  holder.gate.close()
  await until(() => waiting.gate.closed)
  assert.equal(waiting.claims().length, 1)
  assert.equal(waiting.ready(), 0)
})
