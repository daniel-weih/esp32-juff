import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { ProfileStore } from './manager/profiles.mjs'
import { ServiceManager } from './manager/services.mjs'
import { createManagerServer } from './manager/server.mjs'

const root = fileURLToPath(new URL('../../', import.meta.url))
const port = Number(process.env.JUFF_ADMIN_PORT || 3210)
if (!Number.isInteger(port) || port < 1024 || port > 65535) throw new Error('JUFF_ADMIN_PORT must be 1024–65535')
const gateway = new URL(process.env.QWEN_GATEWAY_URL || 'http://127.0.0.1:3101')
const reservedPorts = [port, Number(gateway.port || (gateway.protocol === 'https:' ? 443 : 80))]
const store = new ProfileStore(path.join(root, 'host/.local/devices.json'), reservedPorts)
await store.load()
const manager = new ServiceManager({ root, store })
const app = createManagerServer(manager, { publicDirectory: path.join(root, 'host/public'), port })
let stopping = false
async function stop() {
  if (stopping) return
  stopping = true
  await app.close()
}
process.once('SIGINT', () => void stop())
process.once('SIGTERM', () => void stop())
try {
  const address = await app.start()
  console.log(`JUFF manager ready: http://127.0.0.1:${address.port}`)
  console.log('Voice services stay stopped until started from the dashboard.')
} catch (error) {
  console.error(`JUFF manager failed: ${error.message}`)
  process.exitCode = 1
  await manager.close()
}
