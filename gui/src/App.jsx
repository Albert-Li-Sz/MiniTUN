import { useCallback, useEffect, useMemo, useState } from 'react'
import { api } from './api.js'
import { CloseIcon, DiagnoseIcon, OverviewIcon, PeerIcon, PlusIcon, RefreshIcon, ServerIcon, TunnelIcon } from './icons.jsx'

const navigation = [
  ['overview', '概览', OverviewIcon],
  ['servers', '服务器', ServerIcon],
  ['tunnels', '隧道', TunnelIcon],
  ['p2p', 'P2P', PeerIcon],
  ['diagnostics', '诊断', DiagnoseIcon],
]

const modeLabels = { tcp: 'TCP', udp: 'UDP', socks5: 'SOCKS5', p2p: 'P2P' }

function Logo() {
  return <div className="brand"><img src="/logo.svg" alt=""/><span>MiniTun</span></div>
}

function State({ value }) {
  const tone = ['online', 'active', 'enabled', 'ready'].includes(value) ? 'good'
    : ['failed', 'error', 'backoff'].includes(value) ? 'bad'
      : 'neutral'
  const labels = { online: '在线', active: '活跃', enabled: '启用', pending: '等待', registering: '注册中', disabled: '已停止', failed: '错误', error: '错误', backoff: '重试中' }
  return <span className={`state state-${tone}`}>{labels[value] || value || '未知'}</span>
}

function Summary({ label, value, detail, color }) {
  return <div className="summary"><span className="summary-mark" style={{ '--mark': color }}/><div><div className="summary-label">{label}</div><strong>{value}</strong><div className="summary-detail">{detail}</div></div></div>
}

function EmptyRow({ columns, children = '暂无数据' }) {
  return <tr><td className="empty" colSpan={columns}>{children}</td></tr>
}

function CreateDrawer({ open, servers, onClose, onCreated }) {
  const [mode, setMode] = useState('tcp')
  const [server, setServer] = useState('')
  const [name, setName] = useState('')
  const [localHost, setLocalHost] = useState('127.0.0.1')
  const [localPort, setLocalPort] = useState('')
  const [remoteHost, setRemoteHost] = useState('')
  const [remotePort, setRemotePort] = useState('')
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')

  useEffect(() => {
    if (!server && servers.length) setServer(servers[0].id)
  }, [server, servers])

  async function submit(event) {
    event.preventDefault()
    setSaving(true)
    setError('')
    try {
      const input = {
        server,
        protocol: mode,
        remote_port: Number(remotePort),
        ...(name ? { name } : {}),
        ...(remoteHost ? { remote_host: remoteHost } : {}),
        ...(mode === 'socks5' ? {} : { local_host: localHost, local_port: Number(localPort) }),
      }
      await api.createTunnel(input)
      setName('')
      setLocalPort('')
      setRemotePort('')
      await onCreated()
    } catch (failure) {
      setError(failure.message)
    } finally {
      setSaving(false)
    }
  }

  return <aside className={`drawer ${open ? 'drawer-open' : ''}`} aria-hidden={!open} inert={open ? undefined : true}>
    <div className="drawer-head"><h2>新建隧道</h2><button className="icon-button" onClick={onClose} aria-label="关闭"><CloseIcon/></button></div>
    <form onSubmit={submit}>
      <label>模式<select value={mode} onChange={(event) => setMode(event.target.value)}><option value="tcp">TCP</option><option value="udp">UDP</option><option value="socks5">SOCKS5</option><option value="p2p">P2P</option></select></label>
      <label>服务器<select value={server} onChange={(event) => setServer(event.target.value)} required><option value="" disabled>选择服务器</option>{servers.map((item) => <option value={item.id} key={item.id}>{item.name || item.id} · {item.endpoint}</option>)}</select></label>
      <label>名称（可选）<input value={name} onChange={(event) => setName(event.target.value)} maxLength={64} placeholder="例如 game-udp"/></label>
      {mode !== 'socks5' && <div className="field-group"><span>本地目标</span><div className="endpoint-fields"><input aria-label="本地目标主机" value={localHost} onChange={(event) => setLocalHost(event.target.value)} required/><input aria-label="本地目标端口" type="number" min="1" max="65535" value={localPort} onChange={(event) => setLocalPort(event.target.value)} placeholder="端口" required/></div></div>}
      <div className="field-group"><span>公网绑定</span><div className="endpoint-fields"><input aria-label="公网绑定主机" value={remoteHost} onChange={(event) => setRemoteHost(event.target.value)} placeholder={mode === 'socks5' ? '127.0.0.1' : '0.0.0.0'}/><input aria-label="公网绑定端口" type="number" min="1" max="65535" value={remotePort} onChange={(event) => setRemotePort(event.target.value)} placeholder="端口" required/></div><small>{mode === 'socks5' ? 'SOCKS5 为安全起见只允许绑定 loopback。' : '留空主机时使用默认绑定地址。'}</small></div>
      {error && <div className="form-error" role="alert">{error}</div>}
      <div className="drawer-actions"><button type="button" className="button-secondary" onClick={onClose}>取消</button><button className="button-primary" disabled={saving || !server}>{saving ? '创建中…' : '创建'}</button></div>
    </form>
  </aside>
}

export function App() {
  const [section, setSection] = useState('overview')
  const [drawer, setDrawer] = useState(true)
  const [status, setStatus] = useState(null)
  const [servers, setServers] = useState([])
  const [tunnels, setTunnels] = useState([])
  const [diagnostics, setDiagnostics] = useState(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')

  const load = useCallback(async () => {
    setError('')
    try {
      const [statusResult, serverResult, tunnelResult] = await Promise.all([api.status(), api.servers(), api.tunnels()])
      setStatus(statusResult.status || statusResult)
      setServers(serverResult.servers || [])
      setTunnels(tunnelResult.tunnels || [])
      if (section === 'diagnostics') setDiagnostics(await api.diagnostics())
    } catch (failure) {
      setError(failure.message)
    } finally {
      setLoading(false)
    }
  }, [section])

  useEffect(() => { load() }, [load])

  const visibleTunnels = useMemo(() => section === 'p2p' ? tunnels.filter((item) => item.protocol === 'p2p') : tunnels, [section, tunnels])
  const showServers = section === 'overview' || section === 'servers'
  const showTunnels = section === 'overview' || section === 'tunnels' || section === 'p2p'

  async function toggleTunnel(tunnel) {
    const action = tunnel.desired_state === 'active' ? 'disable' : 'enable'
    try { await api.tunnelAction(tunnel.id, action); await load() } catch (failure) { setError(failure.message) }
  }

  return <div className="app-shell">
    <aside className="sidebar"><Logo/><nav aria-label="主导航">{navigation.map(([id, label, Icon]) => <button key={id} aria-label={label} className={section === id ? 'nav-active' : ''} onClick={() => setSection(id)}><Icon/><span>{label}</span></button>)}</nav><div className="sidebar-status"><span className={`status-dot ${error ? 'dot-error' : ''}`}/><div><strong>{error ? '守护进程不可用' : '守护进程运行中'}</strong><small>本机控制台</small></div></div></aside>
    <main className={`workspace ${drawer ? 'with-drawer' : ''}`}>
      <header><div><h1>MiniTun 控制台</h1><div className="daemon-line"><span className={`status-dot ${error ? 'dot-error' : ''}`}/>{error ? '连接异常' : '守护进程在线'}</div></div><div className="header-actions"><button className="icon-button" onClick={load} aria-label="刷新" disabled={loading}><RefreshIcon/></button><button className="button-primary" onClick={() => setDrawer(true)}><PlusIcon/>新建隧道</button></div></header>
      <div className="content">
        {error && <div className="banner-error" role="alert"><strong>无法读取 daemon 状态</strong><span>{error}</span><button onClick={load}>重试</button></div>}
        {section === 'diagnostics' ? <section><div className="section-title"><h2>诊断</h2></div><pre className="diagnostics">{diagnostics ? JSON.stringify(diagnostics, null, 2) : loading ? '正在读取诊断信息…' : '暂无诊断信息'}</pre></section> : <>
          <section className="summaries"><Summary label="在线服务器" value={status?.servers?.online ?? 0} detail={`全部 ${status?.servers?.total ?? servers.length} 台`} color="#22c55e"/><Summary label="活跃隧道" value={status?.tunnels?.active ?? 0} detail={`全部 ${status?.tunnels?.total ?? tunnels.length} 条`} color="#3b82f6"/><Summary label="活跃连接" value={status?.runtime?.connections?.active ?? 0} detail="实时连接" color="#8b5cf6"/><Summary label="空闲工作线程" value={status?.runtime?.workers?.idle ?? 0} detail={`活跃 ${status?.runtime?.workers?.active ?? 0}`} color="#f59e0b"/></section>
          {showServers && <section><div className="section-title"><h2>服务器状态</h2>{section === 'overview' && <button className="text-button" onClick={() => setSection('servers')}>查看全部</button>}</div><div className="table-wrap"><table><thead><tr><th>名称</th><th>端点</th><th>延迟</th><th>状态</th><th>隧道</th></tr></thead><tbody>{servers.length ? servers.map((server) => <tr key={server.id}><td><span className="row-title"><i className="status-dot"/>{server.name || server.id}</span></td><td className="mono">{server.endpoint}</td><td>{server.latency_ms == null ? '—' : `${server.latency_ms} ms`}</td><td><State value={server.actual_state}/></td><td>{server.tunnel_count ?? 0}</td></tr>) : <EmptyRow columns={5}>{loading ? '正在读取服务器…' : '尚未配置服务器'}</EmptyRow>}</tbody></table></div></section>}
          {showTunnels && <section><div className="section-title"><h2>{section === 'p2p' ? 'P2P 连接' : '最近隧道'}</h2></div><div className="table-wrap"><table><thead><tr><th>名称</th><th>模式</th><th>服务器</th><th>本地目标</th><th>公网端点</th><th>状态</th><th><span className="sr-only">操作</span></th></tr></thead><tbody>{visibleTunnels.length ? visibleTunnels.map((tunnel) => <tr key={tunnel.id}><td><span className="row-title"><i className={`status-dot ${tunnel.actual_state === 'failed' ? 'dot-error' : ''}`}/>{tunnel.name || tunnel.id}</span></td><td><span className={`mode mode-${tunnel.protocol}`}>{modeLabels[tunnel.protocol] || tunnel.protocol}</span></td><td>{tunnel.server_name || tunnel.server_id}</td><td className="mono">{tunnel.protocol === 'socks5' ? '动态目标' : tunnel.local_endpoint}</td><td className="mono endpoint-link">{tunnel.remote_endpoint}</td><td><State value={tunnel.actual_state}/></td><td><button className="row-action" onClick={() => toggleTunnel(tunnel)}>{tunnel.desired_state === 'active' ? '停用' : '启用'}</button></td></tr>) : <EmptyRow columns={7}>{loading ? '正在读取隧道…' : section === 'p2p' ? '尚未建立 P2P 连接' : '尚未创建隧道'}</EmptyRow>}</tbody></table></div></section>}
        </>}
      </div>
    </main>
    <CreateDrawer open={drawer} servers={servers} onClose={() => setDrawer(false)} onCreated={load}/>
  </div>
}
