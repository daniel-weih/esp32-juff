import { createHash, randomUUID } from 'node:crypto'
import { mkdir, readFile, rename, rm, writeFile } from 'node:fs/promises'
import { createRequire } from 'node:module'
import path from 'node:path'
import { pathToFileURL } from 'node:url'
import { ManagerError } from './profiles.mjs'

export const MAX_ASSISTANT_CHARS = 4000 // Qwen Audio Agent 1.11's profile limit.
export const XIAOZHI_PROFILE = `## 身份与称呼

你是 JUFF 设备里的语音伙伴，默认昵称叫“小智”。用户说“你好小智”是在唤醒或称呼你，自然回应即可。
“小智”是用户对你的称呼，Qwen／千问是提供语音与智能能力的技术来源，两者不冲突。不要因为用户叫你“小智”而纠正对方说“我不是小智，我是千问”。用户问你叫什么时，可以说“我叫小智，是你的 JUFF 语音伙伴”；问到所用模型或技术来源时，如实回答。
如果用户明确为你起了新名字，尊重用户的称呼。

## 性格

温和、机灵、可靠，有一点可爱，也有自己的判断。像一个熟悉的桌面伙伴，不刻意卖萌，不讨好，不假装真人。

## 说话方式

默认用自然、简短的中文交流，先说重点，一次说清一件事。适合听的口语表达，少用长列表；需要详细解释时再展开。被唤醒时简短回应，不反复自我介绍。
`

const revision = content => createHash('sha256').update(content).digest('hex')

// Use the installed, pinned agent's read-only resolver so config.env, XDG,
// QWAUDIO_DATA_DIR and explicit profile paths match the actual CLI gateway.
// Resolve lazily: simply opening the dashboard must not initialize Qwen data.
export async function resolveAssistantFile(env = process.env) {
  try {
    const require = createRequire(import.meta.url)
    const packageRoot = path.dirname(require.resolve('qwen-audio-agent/package.json'))
    const { loadRuntimeEnvironment } = await import(pathToFileURL(path.join(packageRoot, 'shared/runtime-environment.mjs')))
    const runtimeEnv = { ...env }
    const root = runtimeEnv.QWEN_AUDIO_AGENT_RUNTIME_ROOT || packageRoot
    const runtime = loadRuntimeEnvironment({ root, env: runtimeEnv, readOnly: true })
    return runtimeEnv.QWEN_AUDIO_AGENT_ASSISTANT_PROFILE_PATH
      ? path.resolve(root, runtimeEnv.QWEN_AUDIO_AGENT_ASSISTANT_PROFILE_PATH)
      : runtime.assistantProfilePath
  } catch {
    throw new ManagerError('无法读取 Qwen 助手配置位置，请检查 Qwen 安装与本机配置', 503)
  }
}

export class AssistantProfileStore {
  constructor({ env = process.env, file } = {}) { this.env = env; this.file = file }
  async location() { return this.file || resolveAssistantFile(this.env) }
  async read(file) {
    file ||= await this.location()
    let content = ''
    try { content = await readFile(file, 'utf8') }
    catch (error) {
      if (error.code !== 'ENOENT') throw new ManagerError('无法读取本机助手设定，请检查文件权限', 500)
    }
    return { content, revision: revision(content), maxChars: MAX_ASSISTANT_CHARS, template: XIAOZHI_PROFILE }
  }
  async save(input) {
    if (!input || Object.keys(input).some(key => !['content', 'revision', 'reconnect'].includes(key))
      || typeof input.content !== 'string' || !input.content.trim()
      || [...input.content].length > MAX_ASSISTANT_CHARS
      || /[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(input.content)) {
      throw new ManagerError('助手设定需要 1–4000 个字符，且不能包含控制字符')
    }
    if (typeof input.revision !== 'string' || !/^[a-f0-9]{64}$/.test(input.revision)) throw new ManagerError('请重新读取助手设定后再保存', 409)
    if (input.reconnect !== undefined && typeof input.reconnect !== 'boolean') throw new ManagerError('重连选项无效')
    const file = await this.location()
    const before = await this.read(file)
    if (input.revision !== before.revision) throw new ManagerError('助手设定已在其他页面或文件中修改，请重新读取后再保存', 409)
    const content = input.content
    if (content === before.content) return before
    const temporary = `${file}.${randomUUID()}.tmp`
    const backup = `${file}.previous`
    const backupTemporary = `${backup}.${randomUUID()}.tmp`
    try {
      await mkdir(path.dirname(file), { recursive: true, mode: 0o700 })
      if (before.content) {
        await writeFile(backupTemporary, before.content, { mode: 0o600, flag: 'wx' })
        await rename(backupTemporary, backup)
      }
      await writeFile(temporary, content, { mode: 0o600, flag: 'wx' })
      await rename(temporary, file)
    } catch {
      throw new ManagerError('助手设定保存失败，请检查本机文件权限和磁盘空间', 500)
    } finally {
      await Promise.all([temporary, backupTemporary].map(item => rm(item, { force: true }).catch(() => {})))
    }
    return { content, revision: revision(content), maxChars: MAX_ASSISTANT_CHARS, template: XIAOZHI_PROFILE }
  }
}
