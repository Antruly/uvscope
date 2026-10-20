export type PreviewKind = 'html' | 'image' | 'audio' | 'video' | 'pdf' | 'none'

export interface HeaderLike {
  name: string
  value: string
}

export function contentTypeOf(headers?: HeaderLike[]): string {
  const header = headers?.find((item) => item.name.toLowerCase() === 'content-type')
  return (header?.value || '').trim()
}

export function previewKindOf(contentType: string): PreviewKind {
  const type = contentType.split(';', 1)[0].trim().toLowerCase()
  if (type === 'text/html' || type === 'application/xhtml+xml') return 'html'
  if (type.startsWith('image/')) return 'image'
  if (type.startsWith('audio/')) return 'audio'
  if (type.startsWith('video/')) return 'video'
  if (type === 'application/pdf') return 'pdf'
  return 'none'
}

export function base64ToBytes(value: string): Uint8Array {
  const binary = atob(value)
  const bytes = new Uint8Array(binary.length)
  for (let i = 0; i < binary.length; i += 1) bytes[i] = binary.charCodeAt(i)
  return bytes
}

export function bytesToText(bytes: Uint8Array): string {
  return new TextDecoder('utf-8', { fatal: false }).decode(bytes)
}

function escapeAttribute(value: string): string {
  return value.replace(/&/g, '&amp;').replace(/"/g, '&quot;')
}

const ABSOLUTE_URL = /^(?:[a-z][a-z0-9+.-]*:|#|\/\/)/i

function absoluteUrl(value: string, baseUrl: string): string {
  const trimmed = value.trim()
  if (!trimmed || ABSOLUTE_URL.test(trimmed)) return value
  try {
    return new URL(trimmed, baseUrl).href
  } catch {
    return value
  }
}

function rewriteCssUrls(css: string, baseUrl: string): string {
  const withUrls = css.replace(/url\(\s*(['"]?)([^'")]+)\1\s*\)/gi, (match, quote, url) => {
    if (!url || ABSOLUTE_URL.test(url.trim())) return match
    try {
      return `url(${quote}${new URL(url.trim(), baseUrl).href}${quote})`
    } catch {
      return match
    }
  })
  // `@import "theme.css"` is not a url() token and would otherwise resolve
  // against about:srcdoc.
  return withUrls.replace(/@import\s+(['"])([^'"]+)\1/gi, (match, quote, url) => {
    if (!url || ABSOLUTE_URL.test(url.trim())) return match
    try {
      return `@import ${quote}${new URL(url.trim(), baseUrl).href}${quote}`
    } catch {
      return match
    }
  })
}

/**
 * Resolve relative resources against the response URL. `<base>` cannot be used
 * here: the embedding page's CSP sets `base-uri 'self'`, and srcdoc documents
 * inherit it, so a target `<base>` is rejected. Rewriting the handful of URL
 * attributes keeps the preview useful without widening `base-uri`.
 *
 * DOMParser is inert: it neither executes scripts nor fetches subresources.
 */
function absolutizeHtml(html: string, baseUrl: string): string {
  const doc = new DOMParser().parseFromString(html, 'text/html')
  const urlAttributes = ['src', 'href', 'poster', 'data']
  doc.querySelectorAll('*').forEach((element) => {
    for (const attribute of urlAttributes) {
      const value = element.getAttribute(attribute)
      if (value) element.setAttribute(attribute, absoluteUrl(value, baseUrl))
    }
    const srcset = element.getAttribute('srcset')
    if (srcset) {
      element.setAttribute(
        'srcset',
        srcset
          .split(',')
          .map((candidate) => {
            const [url, ...descriptor] = candidate.trim().split(/\s+/)
            if (!url) return candidate.trim()
            return [absoluteUrl(url, baseUrl), ...descriptor].join(' ')
          })
          .join(', '),
      )
    }
    const style = element.getAttribute('style')
    if (style) element.setAttribute('style', rewriteCssUrls(style, baseUrl))
  })
  doc.querySelectorAll('style').forEach((element) => {
    element.textContent = rewriteCssUrls(element.textContent || '', baseUrl)
  })
  return doc.documentElement.outerHTML
}

/**
 * Wrap an HTML response in the policy that makes the preview a viewer, not a
 * page with our origin's privileges:
 *
 * - the iframe carrying `srcdoc` is sandboxed with no tokens, so scripts, forms
 *   and top-level navigation are disabled;
 * - the CSP below blocks every network fetch unless the caller explicitly
 *   allows remote assets. `script-src 'none'` stays in both modes.
 *
 * A full response document keeps its own doctype/head; we inject the policy
 * after `<head>` when it exists, and otherwise prepend it (the HTML parser
 * moves stray head elements into the head).
 */
export function buildHtmlDocument(
  html: string,
  options: { allowRemote: boolean; baseUrl?: string },
): string {
  const remote = options.allowRemote
    ? "img-src * data: blob:; media-src * data: blob:; style-src * 'unsafe-inline'; " +
      "font-src * data:; frame-src * data: blob:; child-src * data: blob:"
    : "img-src data: blob:; media-src data: blob:; style-src 'unsafe-inline'; font-src data:"
  const csp =
    `default-src 'none'; script-src 'none'; connect-src 'none'; frame-src 'none'; ` +
    `object-src 'none'; form-action 'none'; base-uri 'none'; ${remote}`
  const policy = `<meta http-equiv="Content-Security-Policy" content="${escapeAttribute(csp)}">`
  const source =
    options.allowRemote && options.baseUrl ? absolutizeHtml(html, options.baseUrl) : html
  const head = policy

  if (/<head[^>]*>/i.test(source)) {
    return source.replace(/<head[^>]*>/i, (match) => match + head)
  }
  if (/<html[^>]*>/i.test(source)) {
    return source.replace(/<html[^>]*>/i, (match) => match + `<head>${head}</head>`)
  }
  return head + source
}
