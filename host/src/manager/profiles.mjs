import { mkdir, readFile, rename, writeFile } from 'node:fs/promises'
import path from 'node:path'
import { randomUUID } from 'node:crypto'

export const BOARDS = {
  'waveshare-lcd-1.54': { label: '1.54 英寸', resolution: '240 × 240' },
  'waveshare-lcd-3.5': { label: '3.5 英寸', resolution: '320 × 480' },
}

export class ManagerError extends Error {
  constructor(message, status = 400) { super(message); this.status = status }
}

const defaults = () => [
  { id: 'small', name: '小屏 JUFF', board: 'waveshare-lcd-1.54', bleName: '', bridgePort: 8766, sessionId: 'esp32-small' },
  { id: 'large', name: '大屏 JUFF', board: 'waveshare-lcd-3.5', bleName: '', bridgePort: 8765, sessionId: 'esp32-main' },
]

export function validateProfiles(profiles, reservedPorts = []) {
  if (!Array.isArray(profiles) || profiles.length > 8) throw new ManagerError('最多可以保存 8 台设备')
  const ids = new Set(), ports = new Set(reservedPorts), names = new Set(), sessions = new Set()
  return profiles.map(item => {
    if (!item || typeof item !== 'object') throw new ManagerError('设备配置格式不正确')
    const { id, name, board, bridgePort, sessionId } = item
    const bleName = typeof item.bleName === 'string' ? item.bleName.trim() : item.bleName
    if (typeof id !== 'string' || !/^[a-z][a-z0-9-]{0,31}$/.test(id) || ids.has(id)) throw new ManagerError('设备编号重复或无效')
    if (typeof name !== 'string' || !name.trim() || name.length > 40 || /[\x00-\x1f]/.test(name)) throw new ManagerError('设备名称需要 1–40 个字符')
    if (!Object.hasOwn(BOARDS, board)) throw new ManagerError('请选择支持的硬件型号')
    if (typeof bleName !== 'string' || bleName.length > 64 || /[\x00-\x1f]/.test(bleName)) throw new ManagerError('蓝牙名称无效')
    if (bleName && (!bleName.startsWith('JUFF-') || names.has(bleName.toLowerCase()))) throw new ManagerError('蓝牙名称需以 JUFF- 开头，且不能重复绑定')
    if (!Number.isInteger(bridgePort) || bridgePort < 1024 || bridgePort > 65535 || ports.has(bridgePort)) throw new ManagerError('Bridge 端口需在 1024–65535 之间，且不能与其他服务重复')
    if (typeof sessionId !== 'string' || !/^[a-zA-Z0-9_-]{1,48}$/.test(sessionId) || sessions.has(sessionId)) throw new ManagerError('会话编号只能包含字母、数字、下划线和短横线，且不能重复')
    ids.add(id); ports.add(bridgePort); sessions.add(sessionId)
    if (bleName) names.add(bleName.toLowerCase())
    return { id, name: name.trim(), board, bleName: bleName.trim(), bridgePort, sessionId }
  })
}

export class ProfileStore {
  constructor(file, reservedPorts = []) { this.file = file; this.reservedPorts = reservedPorts; this.profiles = [] }
  async load() {
    let data
    try { data = JSON.parse(await readFile(this.file, 'utf8')) }
    catch (error) {
      if (error.code !== 'ENOENT') throw new ManagerError('本地设备配置无法读取，请检查 host/.local/devices.json', 500)
      data = { version: 1, devices: defaults() }
    }
    if (data.version !== 1) throw new ManagerError('本地设备配置版本不支持', 500)
    this.profiles = validateProfiles(data.devices, this.reservedPorts)
    return this.list()
  }
  list() { return this.profiles.map(item => ({ ...item })) }
  async save(profiles) {
    const validated = validateProfiles(profiles, this.reservedPorts)
    await mkdir(path.dirname(this.file), { recursive: true, mode: 0o700 })
    const temporary = `${this.file}.${randomUUID()}.tmp`
    await writeFile(temporary, JSON.stringify({ version: 1, devices: validated }, null, 2) + '\n', { mode: 0o600 })
    await rename(temporary, this.file)
    this.profiles = validated
    return this.list()
  }
}
