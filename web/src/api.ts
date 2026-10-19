export class ApiError extends Error {
  status: number
  code?: string
  payload?: unknown

  constructor(status: number, message: string, code?: string, payload?: unknown) {
    super(message)
    this.status = status
    this.code = code
    this.payload = payload
  }
}

type RequestOptions = Omit<RequestInit, 'body'> & { body?: unknown }

export async function api<T>(path: string, options: RequestOptions = {}): Promise<T> {
  const headers = new Headers(options.headers)
  let body: BodyInit | undefined
  if (options.body !== undefined) {
    if (options.body instanceof FormData || options.body instanceof Blob) {
      body = options.body
    } else {
      headers.set('content-type', 'application/json')
      body = JSON.stringify(options.body)
    }
  }
  headers.set('accept', 'application/json')

  let response: Response
  try {
    response = await fetch(path, {
      ...options,
      headers,
      body,
      credentials: 'same-origin',
    })
  } catch (error) {
    throw new ApiError(0, error instanceof Error ? error.message : 'network request failed')
  }

  const text = await response.text()
  let payload: unknown = undefined
  if (text) {
    try {
      payload = JSON.parse(text)
    } catch {
      payload = text
    }
  }

  if (!response.ok) {
    const record = payload && typeof payload === 'object' ? (payload as Record<string, unknown>) : {}
    const message =
      typeof record.error === 'string'
        ? record.error
        : response.status === 401
          ? 'authentication required'
          : `request failed with HTTP ${response.status}`
    const error = new ApiError(response.status, message, typeof record.code === 'string' ? record.code : undefined, payload)
    if (response.status === 401) {
      window.dispatchEvent(new CustomEvent('uvscope:unauthorized'))
    }
    throw error
  }

  return payload as T
}

export function get<T>(path: string): Promise<T> {
  return api<T>(path)
}

export function post<T>(path: string, body?: unknown): Promise<T> {
  return api<T>(path, { method: 'POST', body })
}

export function put<T>(path: string, body?: unknown): Promise<T> {
  return api<T>(path, { method: 'PUT', body })
}

export function patch<T>(path: string, body?: unknown): Promise<T> {
  return api<T>(path, { method: 'PATCH', body })
}

export function del<T>(path: string): Promise<T> {
  return api<T>(path, { method: 'DELETE' })
}

export function errorMessage(error: unknown): string {
  if (error instanceof ApiError) return error.message
  if (error instanceof Error) return error.message
  return String(error)
}
