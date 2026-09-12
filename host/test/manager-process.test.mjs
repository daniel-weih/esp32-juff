import assert from 'node:assert/strict'
import net from 'node:net'
import { once } from 'node:events'
import { spawn } from 'node:child_process'
import { mkdtemp, mkdir, readFile, rm } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import { ProfileStore } from '../src/manager/profiles.mjs'
import { ServiceManager } from '../src/manager/services.mjs'
import { AssistantProfileStore } from '../src/manager/assistant.mjs'

test('real owned worker and its BLE descendant exit while the shared gateway stays alive', { timeout: 15000 }, async t => {
  const reservation = net.createServer().listen(0, '127.0.0.1')
  await once(reservation, 'listening')
  const port = reservation.address().port
  await new Promise(resolve => reservation.close(resolve))
  const root = await mkdtemp(path.join(os.tmpdir(), 'juff-process-test-'))
  await mkdir(path.join(root, 'host'))
  const childPID = path.join(root, 'ble-child.pid')
  const store = new ProfileStore(path.join(root, 'devices.json'))
  await store.load()
  await store.save(store.list().map(item => ({ ...item, bleName: `JUFF-${item.id}` })))
  const worker = fileURLToPath(new URL('./fixtures/managed-worker.mjs', import.meta.url))
  const manager = new ServiceManager({ root, store, checkFiles: false,
    assistantStore: new AssistantProfileStore({ file: path.join(root, 'ASSISTANT.md') }),
    env: { ...process.env, JUFF_DEVICE_TOKEN: 'synthetic-device-token', QWEN_GATEWAY_URL: `http://127.0.0.1:${port}` },
    spawnImpl(executable, args, options) {
      const kind = executable.endsWith('qwenaudio') ? 'gateway' : options.env.JUFF_SERVICE_MODE
      return spawn(process.execPath, [worker], { ...options, env: { ...options.env, FIXTURE_KIND: kind, FIXTURE_CHILD_PID: childPID } })
    },
  })
  t.after(async () => { await manager.close(); await rm(root, { recursive: true, force: true }) })
  await manager.command('small-ble', 'start')
  const descendant = Number(await readFile(childPID, 'utf8'))
  assert.doesNotThrow(() => process.kill(descendant, 0))
  await manager.command('small-ble', 'stop')
  await manager.waitUntil(() => { try { process.kill(descendant, 0); return false } catch (error) { return error.code === 'ESRCH' } }, 3000, 'BLE descendant survived stop')
  const health = await fetch(`http://127.0.0.1:${port}/api/health`)
  assert.equal(health.status, 200)
  await manager.command('all', 'stop')
  await assert.rejects(fetch(`http://127.0.0.1:${port}/api/health`))
})
