import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { once } from 'node:events'
import { copyFile, mkdir, mkdtemp, rm, symlink, writeFile } from 'node:fs/promises'
import http from 'node:http'
import os from 'node:os'
import path from 'node:path'
import test from 'node:test'
import { setTimeout as delay } from 'node:timers/promises'
import { fileURLToPath } from 'node:url'
import { WebSocketServer } from 'ws'
import { LogStore, readLines } from '../src/manager/logs.mjs'

test('managed BLE worker keeps multiline transcripts out of operational logs', { timeout: 15000 }, async t => {
  const root = await mkdtemp(path.join(os.tmpdir(), 'juff-voice-log-test-'))
  const host = path.join(root, 'host')
  const bleDirectory = path.join(root, 'macos/build/JuffBLE.app/Contents/MacOS')
  const server = http.createServer((request, response) => {
    response.writeHead(200, { 'content-type': 'application/json' })
    response.end(JSON.stringify({ backend: {}, realtimeModel: 'test-model' }))
  })
  const sockets = new WebSocketServer({ server })
  let child, exited, output = ''
  const logs = new LogStore()
  t.after(async () => {
    if (child?.pid) {
      try { process.kill(-child.pid, 'SIGTERM') } catch {}
      if (!await Promise.race([exited.then(() => true), delay(2000).then(() => false)])) {
        try { process.kill(-child.pid, 'SIGKILL') } catch {}
        await exited
      }
    }
    for (const socket of sockets.clients) socket.terminate()
    await new Promise(resolve => sockets.close(resolve))
    server.closeIdleConnections()
    await new Promise(resolve => server.close(resolve))
    await rm(root, { recursive: true, force: true })
  })
  await mkdir(path.join(host, 'src'), { recursive: true })
  await mkdir(bleDirectory, { recursive: true })
  for (const name of ['run.mjs', 'config.mjs', 'device-bridge.mjs', 'gateway-client.mjs', 'conversation-client.mjs', 'playback-diagnostics.mjs', 'wake-acknowledgement.mjs']) {
    await copyFile(new URL(`../src/${name}`, import.meta.url), path.join(host, 'src', name))
  }
  await mkdir(path.join(host, 'assets'))
  await copyFile(new URL('../assets/wake-ack.pcm', import.meta.url), path.join(host, 'assets/wake-ack.pcm'))
  await symlink(fileURLToPath(new URL('../node_modules', import.meta.url)), path.join(host, 'node_modules'), 'dir')
  await writeFile(path.join(bleDirectory, 'JuffBLE'),
    '#!/usr/bin/env node\nprocess.stdin.resume(); process.stdout.write("STATE audio-connected:fixture\\n");\n', { mode: 0o700 })
  sockets.on('connection', socket => socket.once('message', () => {
    socket.send(JSON.stringify({ type: 'transcript.final', role: 'user', content: 'private-first-line\nprivate-second-line' }))
    socket.send(JSON.stringify({ type: 'response.done', responseId: 'test-response' }))
  }))
  server.listen(0, '127.0.0.1')
  await once(server, 'listening')
  child = spawn(process.execPath, ['src/run.mjs'], {
    cwd: host, detached: true, stdio: ['ignore', 'pipe', 'pipe'],
    env: { PATH: process.env.PATH, JUFF_SERVICE_MODE: 'ble', JUFF_BLE_DEVICE: 'JUFF-TEST',
      JUFF_DEVICE_TOKEN: 'synthetic-device-token', QWEN_GATEWAY_URL: `http://127.0.0.1:${server.address().port}` },
  })
  exited = once(child, 'exit')
  for (const stream of [child.stdout, child.stderr]) {
    stream.on('data', chunk => { output += chunk })
    readLines(stream, line => logs.add('small-ble', line))
  }
  const deadline = Date.now() + 5000
  while (!output.includes('QWEN EVENT response.done: test-response') && Date.now() < deadline) {
    assert.equal(child.exitCode, null, output)
    await delay(10)
  }
  assert.ok(output.includes('Juff ready: test-model; BLE only'), output)
  assert.ok(output.includes('BLE voice bridge connected:'), output)
  assert.ok(!output.includes('BLE voice ready:'), 'An open local socket is not cloud voice readiness')
  assert.ok(logs.read().some(entry => entry.message === 'QWEN EVENT response.done: test-response'), output)
  for (const value of ['private-first-line', 'private-second-line']) {
    assert.ok(!output.includes(value), 'Managed stdout must omit the complete transcript')
    assert.ok(!JSON.stringify(logs.read()).includes(value), 'Dashboard logs must omit every transcript line')
  }
})
