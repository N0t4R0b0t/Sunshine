import { notifyKey } from './Notification.vue'

/**
 * The set of error messages that indicate a CSRF validation failure.
 */
const CSRF_ERRORS = new Set(['Missing CSRF token', 'Invalid CSRF token', 'CSRF token expired'])

/**
 * HTTP methods that mutate server state and therefore need a CSRF token attached.
 * The backend only requires one when the request's Origin/Referer doesn't match
 * an allowed origin (e.g. accessed through a forwarded/proxied URL rather than
 * directly on localhost) - sending the header unconditionally is harmless when
 * the backend doesn't need it, since it validates same-origin first.
 */
const MUTATING_METHODS = new Set(['POST', 'PUT', 'PATCH', 'DELETE'])

let cachedCsrfToken = null
let csrfTokenPromise = null

/**
 * Fetch (and cache) a CSRF token from the server. Concurrent callers share the
 * same in-flight request instead of triggering duplicate fetches.
 *
 * @returns {Promise<string|null>} The token, or `null` if it could not be fetched.
 */
async function getCsrfToken() {
  if (cachedCsrfToken) {
    return cachedCsrfToken
  }
  if (!csrfTokenPromise) {
    csrfTokenPromise = fetch('./api/csrf-token')
      .then((r) => (r.ok ? r.json() : null))
      .then((data) => {
        cachedCsrfToken = data?.csrf_token ?? null
        return cachedCsrfToken
      })
      .catch((e) => {
        console.debug('apiFetch: failed to fetch CSRF token', e)
        return null
      })
      .finally(() => {
        csrfTokenPromise = null
      })
  }
  return csrfTokenPromise
}

/**
 * Wrapper around the native fetch that automatically attaches a CSRF token to
 * mutating requests and detects CSRF errors (HTTP 400 with a known CSRF error
 * message), retrying once with a freshly fetched token before giving up and
 * showing a notification.
 *
 * @param {string} url - The URL to fetch.
 * @param {RequestInit} [options] - Standard fetch options.
 * @param {boolean} [_isRetry] - Internal: whether this call is already a CSRF-triggered retry.
 * @returns {Promise<Response>} The fetch Response.
 */
export async function apiFetch(url, options, _isRetry = false) {
  const method = (options?.method || 'GET').toUpperCase()

  let finalOptions = options
  if (MUTATING_METHODS.has(method)) {
    const token = await getCsrfToken()
    if (token) {
      finalOptions = {
        ...options,
        headers: {
          ...(options?.headers || {}),
          'X-CSRF-Token': token,
        },
      }
    }
  }

  const response = await fetch(url, finalOptions)

  if (response.status === 400) {
    let body = null
    try {
      body = await response.clone().json()
    } catch (e) {
      console.debug('apiFetch: response body is not JSON', e)
    }

    if (body && CSRF_ERRORS.has(body.error)) {
      // The cached token may be missing/stale (e.g. server restarted, or this is
      // the very first mutating request this page has made) - refresh and retry
      // once before surfacing an error to the user.
      cachedCsrfToken = null
      if (!_isRetry && MUTATING_METHODS.has(method)) {
        return apiFetch(url, options, true)
      }
      notifyKey.error('_common.csrf_error_desc', '_common.csrf_error')
    }
  }

  return response
}
