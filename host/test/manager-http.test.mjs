import assert from 'node:assert/strict'
import http from 'node:http'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import { mkdtemp, rm } from 'node:fs/promises'
import os from 'node:os'
import { setTimeout as delay } from 'node:timers/promises'
import { createManagerServer } from '../src/manager/server.mjs'
import { LogStore } from '../src/manager/logs.mjs'
import { validateProfiles } from '../src/manager/profiles.mjs'
import { AssistantProfileStore } from '../src/manager/assistant.mjs'

async function fixture(t) {
  const calls = [], logs = new LogStore()
  const directory = await mkdtemp(path.join(os.tmpdir(), 'juff-assistant-http-'))
  t.after(() => rm(directory, { recursive: true, force: true }))
  const assistant = new AssistantProfileStore({ file: path.join(directory, 'ASSISTANT.md') })
  let profiles = []
  const manager = {
    logs, refresh: async () => {}, close: async () => {},
    snapshot: () => ({ devices: profiles, services: [], logs: logs.read() }),
    command: async (id, action) => { calls.push([id, action]); return manager.snapshot() },
    updateProfiles: async operation => { profiles = validateProfiles(operation(profiles)); return manager.snapshot() },
    readAssistant: () => assistant.read(), saveAssistant: input => assistant.save(input),
  }
  const app = createManagerServer(manager, { publicDirectory: fileURLToPath(new URL('../public', import.meta.url)), port: 0 })
  const address = await app.start()
  t.after(() => app.close())
  const origin = `http://127.0.0.1:${address.port}`
  const state = await (await fetch(`${origin}/api/state`)).json()
  const headers = { origin, 'content-type': 'application/json', 'x-juff-csrf': state.csrfToken }
  return { origin, headers, calls, manager }
}

test('assistant API enforces origin, CSRF, revision and content limits without exposing prompts in state', async t => {
  const { origin, headers } = await fixture(t)
  const endpoint = `${origin}/api/assistant`
  const initial = await (await fetch(endpoint)).json()
  const input = { content: '你叫小智，回答简短自然。', revision: initial.revision }
  for (const override of [{ origin: 'https://untrusted.invalid' }, { 'x-juff-csrf': '' }]) {
    assert.equal((await fetch(endpoint, { method: 'PUT', headers: { ...headers, ...override }, body: JSON.stringify(input) })).status, 403)
  }
  assert.equal((await fetch(endpoint, { headers: { origin: 'https://untrusted.invalid' } })).status, 403)
  const response = await fetch(endpoint, { method: 'PUT', headers, body: JSON.stringify(input) })
  assert.equal(response.status, 200)
  const saved = await response.json()
  assert.equal(saved.content, input.content)
  assert.equal((await fetch(endpoint, { method: 'PUT', headers, body: JSON.stringify(input) })).status, 409)
  assert.equal((await fetch(endpoint, { method: 'PUT', headers, body: JSON.stringify({ content: '文'.repeat(4001), revision: saved.revision }) })).status, 400)
  assert.equal((await fetch(endpoint, { method: 'PUT', headers, body: JSON.stringify({ content: 'x'.repeat(33000), revision: saved.revision }) })).status, 413)
  // Four thousand astral characters are 16 KB in UTF-8 before JSON framing.
  assert.equal((await fetch(endpoint, { method: 'PUT', headers, body: JSON.stringify({ content: '😀'.repeat(4000), revision: saved.revision }) })).status, 200)
  assert.ok(!(await (await fetch(`${origin}/api/state`)).text()).includes('😀'))
})

test('dashboard and assets load locally with restrictive browser headers', async t => {
  const { origin } = await fixture(t)
  for (const asset of ['/', '/app.js', '/app.css', '/favicon.svg']) {
    const response = await fetch(origin + asset)
    assert.equal(response.status, 200)
    assert.equal(response.headers.get('cache-control'), 'no-store')
    assert.ok(response.headers.get('content-security-policy').includes("frame-ancestors 'none'"))
    assert.equal(response.headers.get('access-control-allow-origin'), null)
    await response.text()
  }
  assert.equal((await fetch(origin + '/.env')).status, 404)
  assert.equal((await fetch(origin + '/src/admin.mjs')).status, 404)
  assert.equal((await fetch(origin + '/__proto__')).status, 404)
})

test('only same-origin token-bearing JSON requests can control services', async t => {
  const { origin, headers, calls } = await fixture(t)
  const endpoint = `${origin}/api/services/gateway/start`
  for (const override of [{ origin: 'https://untrusted.invalid' }, { 'x-juff-csrf': 'wrong' }, { origin: '' }]) {
    const response = await fetch(endpoint, { method: 'POST', headers: { ...headers, ...override }, body: '{}' })
    assert.equal(response.status, 403)
  }
  assert.equal((await fetch(endpoint)).status, 404)
  assert.equal((await fetch(endpoint, { method: 'POST', headers: { ...headers, 'content-type': 'text/plain' }, body: '{}' })).status, 415)
  assert.deepEqual(calls, [])
  const response = await fetch(endpoint, { method: 'POST', headers, body: '{}' })
  assert.equal(response.status, 200)
  assert.deepEqual(calls, [['gateway', 'start']])
  assert.equal((await fetch(origin + '/api/services/gateway/execute', { method: 'POST', headers, body: '{}' })).status, 404)
})

test('DNS rebinding hosts and foreign-origin reads are rejected', async t => {
  const { origin } = await fixture(t)
  const response = await new Promise((resolve, reject) => {
    const request = http.get(`${origin}/api/state`, { headers: { host: 'untrusted.invalid' } }, response => { response.resume(); resolve(response) })
    request.on('error', reject)
  })
  assert.equal(response.statusCode, 403)
  assert.equal((await fetch(`${origin}/api/state`, { headers: { origin: 'https://untrusted.invalid' } })).status, 403)
})

test('device configuration API validates, updates, and removes profiles', async t => {
  const { origin, headers } = await fixture(t)
  const item = { name: '测试设备', board: 'waveshare-lcd-1.54', bleName: 'JUFF-TEST', bridgePort: 9876, sessionId: 'test-profile' }
  let response = await fetch(`${origin}/api/devices`, { method: 'POST', headers, body: JSON.stringify(item) })
  assert.equal(response.status, 201)
  const id = (await response.json()).devices[0].id
  response = await fetch(`${origin}/api/devices/${id}`, { method: 'PUT', headers, body: JSON.stringify({ ...item, name: '已改名' }) })
  assert.equal((await response.json()).devices[0].name, '已改名')
  response = await fetch(`${origin}/api/devices`, { method: 'POST', headers, body: JSON.stringify({ ...item, name: '重复端口' }) })
  assert.equal(response.status, 400)
  response = await fetch(`${origin}/api/devices/${id}`, { method: 'DELETE', headers, body: '{}' })
  assert.deepEqual((await response.json()).devices, [])
  response = await fetch(`${origin}/api/devices`, { method: 'POST', headers, body: JSON.stringify({ name: 'x'.repeat(17000) }) })
  assert.equal(response.status, 413)
})

test('device names survive UTF-8 characters split across HTTP chunks', async t => {
  const { origin, headers } = await fixture(t)
  const item = { name: '桌面小伙伴', board: 'waveshare-lcd-1.54', bleName: 'JUFF-TEST', bridgePort: 9876, sessionId: 'test-profile' }
  const bytes = Buffer.from(JSON.stringify(item))
  const split = bytes.indexOf(Buffer.from('桌')) + 1
  let request
  const result = new Promise((resolve, reject) => {
    request = http.request(`${origin}/api/devices`, { method: 'POST', headers }, response => {
      let body = ''
      response.setEncoding('utf8')
      response.on('data', chunk => { body += chunk })
      response.on('end', () => resolve({ status: response.statusCode, body: JSON.parse(body) }))
    })
    request.on('error', reject)
  })
  request.write(bytes.subarray(0, split))
  await delay(10)
  request.end(bytes.subarray(split))
  const response = await result
  assert.equal(response.status, 201)
  assert.equal(response.body.devices[0].name, item.name)
})
