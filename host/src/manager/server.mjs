import http from 'node:http'
import { readFile } from 'node:fs/promises'
import { randomBytes, randomUUID, timingSafeEqual } from 'node:crypto'
import path from 'node:path'
import { ManagerError } from './profiles.mjs'

const assets = {
  '/': ['index.html', 'text/html; charset=utf-8'],
  '/app.css': ['app.css', 'text/css; charset=utf-8'],
  '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
  '/favicon.svg': ['favicon.svg', 'image/svg+xml'],
}

export function createManagerServer(manager, { publicDirectory, port = 3210 } = {}) {
  const csrfToken = randomBytes(32).toString('hex')
  let refreshTimer
  const server = http.createServer((request, response) => {
    void handle(request, response).catch(error => {
      const message = error instanceof ManagerError ? error.message : '请求处理失败，请查看后台日志'
      if (!(error instanceof ManagerError)) manager.logs.add('manager', error.message, 'error')
      if (!response.headersSent) json(response, error.status || 500, { error: message })
      else response.end()
    })
  })
  server.requestTimeout = 20000
  server.headersTimeout = 10000
  server.maxHeadersCount = 40

  function json(response, status, body) {
    response.writeHead(status, { 'content-type': 'application/json; charset=utf-8' })
    response.end(JSON.stringify(body))
  }
  async function body(request) {
    if (request.headers['content-type']?.split(';')[0] !== 'application/json') throw new ManagerError('需要 JSON 请求', 415)
    const chunks = []
    let size = 0
    for await (const chunk of request) {
      size += chunk.length
      if (size > 16384) throw new ManagerError('请求内容过大', 413)
      chunks.push(chunk)
    }
    const data = Buffer.concat(chunks).toString('utf8')
    try {
      const value = JSON.parse(data || '{}')
      if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error()
      return value
    } catch { throw new ManagerError('JSON 格式不正确') }
  }
  async function handle(request, response) {
    response.setHeader('cache-control', 'no-store')
    response.setHeader('x-content-type-options', 'nosniff')
    response.setHeader('referrer-policy', 'no-referrer')
    response.setHeader('content-security-policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'")
    const actualPort = server.address()?.port
    const allowedHosts = [`127.0.0.1:${actualPort}`, `localhost:${actualPort}`]
    if (!allowedHosts.includes(request.headers.host)) throw new ManagerError('仅接受本机管理请求', 403)
    const origin = `http://${request.headers.host}`
    if (request.headers.origin && request.headers.origin !== origin) throw new ManagerError('不接受跨站请求', 403)
    if (request.headers['sec-fetch-site'] === 'cross-site') throw new ManagerError('不接受跨站请求', 403)
    const url = new URL(request.url, origin)
    if (request.method === 'GET') {
      if (url.pathname === '/api/state') return json(response, 200, { ...manager.snapshot(), csrfToken })
      if (url.pathname === '/api/logs') return json(response, 200, { entries: manager.logs.read({
        service: url.searchParams.get('service'), after: Math.max(0, Number(url.searchParams.get('after')) || 0),
      }) })
      const asset = Object.hasOwn(assets, url.pathname) ? assets[url.pathname] : null
      if (!asset) throw new ManagerError('页面不存在', 404)
      const data = await readFile(path.join(publicDirectory, asset[0]))
      response.writeHead(200, { 'content-type': asset[1] }); response.end(data); return
    }
    if (!['POST', 'PUT', 'DELETE'].includes(request.method)) throw new ManagerError('不支持的请求方法', 405)
    const token = Buffer.from(String(request.headers['x-juff-csrf'] || ''))
    const expected = Buffer.from(csrfToken)
    if (request.headers.origin !== origin || token.length !== expected.length || !timingSafeEqual(token, expected)) throw new ManagerError('页面会话已失效，请刷新后重试', 403)
    const input = await body(request)
    const action = /^\/api\/services\/([a-z0-9-]+)\/(start|stop|restart)$/.exec(url.pathname)
    if (action && request.method === 'POST') return json(response, 200, await manager.command(action[1], action[2]))
    if (url.pathname === '/api/devices' && request.method === 'POST') {
      const id = `device-${randomUUID().slice(0, 8)}`
      return json(response, 201, await manager.updateProfiles(profiles => [...profiles, { ...input, id }]))
    }
    const device = /^\/api\/devices\/([a-z0-9-]+)$/.exec(url.pathname)
    if (device && ['PUT', 'DELETE'].includes(request.method)) {
      return json(response, 200, await manager.updateProfiles(profiles => {
        if (!profiles.some(profile => profile.id === device[1])) throw new ManagerError('设备不存在', 404)
        return request.method === 'DELETE' ? profiles.filter(profile => profile.id !== device[1])
          : profiles.map(profile => profile.id === device[1] ? { ...input, id: profile.id } : profile)
      }))
    }
    throw new ManagerError('接口不存在', 404)
  }
  return {
    server,
    async start() {
      await new Promise((resolve, reject) => {
        server.once('error', reject)
        server.listen(port, '127.0.0.1', () => { server.off('error', reject); resolve() })
      })
      await manager.refresh()
      refreshTimer = setInterval(() => void manager.refresh().catch(error => manager.logs.add('manager', error.message, 'error')), 2500)
      refreshTimer.unref()
      return server.address()
    },
    async close() {
      clearInterval(refreshTimer)
      await manager.close()
      server.closeIdleConnections()
      await new Promise(resolve => server.close(resolve))
    },
  }
}
