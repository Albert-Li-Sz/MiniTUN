async function request(path, options = {}) {
  const response = await fetch(`/api/v1${path}`, {
    headers: {
      Accept: 'application/json',
      ...(options.body ? { 'Content-Type': 'application/json' } : {}),
      ...options.headers,
    },
    ...options,
  })
  const body = await response.json().catch(() => ({}))
  if (!response.ok) {
    throw new Error(body?.error?.message || body?.message || `请求失败 (${response.status})`)
  }
  return body
}

export const api = {
  status: () => request('/status'),
  servers: () => request('/servers'),
  tunnels: () => request('/tunnels'),
  diagnostics: () => request('/diagnostics'),
  createTunnel: (input) => request('/tunnels', { method: 'POST', body: JSON.stringify(input) }),
  tunnelAction: (id, action) => request(`/tunnels/${encodeURIComponent(id)}/${action}`, { method: 'POST', body: '{}' }),
}
