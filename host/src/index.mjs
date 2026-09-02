import process from 'node:process'
import { readConfig } from './config.mjs'
import { createDeviceBridge } from './device-bridge.mjs'

const config = readConfig()
const bridge = createDeviceBridge(config)
await bridge.start()

let stopping = false
async function stop(signal) {
  if (stopping) return
  stopping = true
  process.stdout.write(`${JSON.stringify({
    time: new Date().toISOString(),
    level: 'info',
    message: 'stopping device bridge',
    signal,
  })}\n`)
  await bridge.close()
}

process.once('SIGINT', () => void stop('SIGINT'))
process.once('SIGTERM', () => void stop('SIGTERM'))
