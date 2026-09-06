// Local subprocess fixture: no Bluetooth, microphone, credentials, or cloud.
import http from 'node:http'
import { spawn } from 'node:child_process'
import { writeFileSync } from 'node:fs'

const kind = process.env.FIXTURE_KIND
if (kind === 'ble') {
  const child = spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], { stdio: 'ignore' })
  writeFileSync(process.env.FIXTURE_CHILD_PID, String(child.pid))
  process.stdout.write('Juff ready: subprocess fixture\nBLE STATE scanning\n')
} else {
  const server = http.createServer((request, response) => {
    response.writeHead(200, { 'content-type': 'application/json' })
    response.end(JSON.stringify(kind === 'gateway' ? { backend: {}, realtimeModel: 'fixture' } : { ok: true, deviceConnected: false }))
  })
  server.listen(Number(process.env.PORT), '127.0.0.1')
}
process.once('SIGTERM', () => process.exit(0))
