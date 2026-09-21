/**
 * Captures keyboard, mouse and gamepad input in the browser and turns it into the small JSON messages
 * that Sunshine's browser streaming client understands (see `translate_input` in
 * `src/webrtc/input_translate.h` for the full list).
 *
 * The mapping tables and message builders are plain functions so they can be tested without a browser.
 * `InputSender` does the DOM and gamepad work, and takes everything it depends on as arguments.
 */

/**
 * Windows virtual-key codes by `KeyboardEvent.code`. Moonlight always sends Windows codes on the wire,
 * whatever platform the client runs on, and `code` names the physical key regardless of layout.
 */
const VK_BY_CODE = {
  Backspace: 0x08,
  Tab: 0x09,
  Enter: 0x0d,
  NumpadEnter: 0x0d,
  Pause: 0x13,
  CapsLock: 0x14,
  Escape: 0x1b,
  Space: 0x20,
  PageUp: 0x21,
  PageDown: 0x22,
  End: 0x23,
  Home: 0x24,
  ArrowLeft: 0x25,
  ArrowUp: 0x26,
  ArrowRight: 0x27,
  ArrowDown: 0x28,
  PrintScreen: 0x2c,
  Insert: 0x2d,
  Delete: 0x2e,
  MetaLeft: 0x5b,
  MetaRight: 0x5c,
  ContextMenu: 0x5d,
  NumpadMultiply: 0x6a,
  NumpadAdd: 0x6b,
  NumpadSubtract: 0x6d,
  NumpadDecimal: 0x6e,
  NumpadDivide: 0x6f,
  NumLock: 0x90,
  ScrollLock: 0x91,
  ShiftLeft: 0xa0,
  ShiftRight: 0xa1,
  ControlLeft: 0xa2,
  ControlRight: 0xa3,
  AltLeft: 0xa4,
  AltRight: 0xa5,
  Semicolon: 0xba,
  Equal: 0xbb,
  Comma: 0xbc,
  Minus: 0xbd,
  Period: 0xbe,
  Slash: 0xbf,
  Backquote: 0xc0,
  BracketLeft: 0xdb,
  Backslash: 0xdc,
  BracketRight: 0xdd,
  Quote: 0xde,
  IntlBackslash: 0xe2,
}

for (let letter = 0; letter < 26; letter++) {
  VK_BY_CODE[`Key${String.fromCharCode(65 + letter)}`] = 0x41 + letter
}
for (let digit = 0; digit < 10; digit++) {
  VK_BY_CODE[`Digit${digit}`] = 0x30 + digit
  VK_BY_CODE[`Numpad${digit}`] = 0x60 + digit
}
for (let fn = 1; fn <= 24; fn++) {
  VK_BY_CODE[`F${fn}`] = 0x6f + fn
}

/**
 * Moonlight controller button flags, keyed by the index of the button in the browser's "standard"
 * gamepad layout. The two triggers (6 and 7) are analogue and are reported separately.
 */
const GAMEPAD_BUTTON_FLAGS = {
  0: 0x1000, // A
  1: 0x2000, // B
  2: 0x4000, // X
  3: 0x8000, // Y
  4: 0x0100, // left bumper
  5: 0x0200, // right bumper
  8: 0x0020, // back
  9: 0x0010, // start
  10: 0x0040, // left stick click
  11: 0x0080, // right stick click
  12: 0x0001, // d-pad up
  13: 0x0002, // d-pad down
  14: 0x0004, // d-pad left
  15: 0x0008, // d-pad right
  16: 0x0400, // guide
}

/** Controllers beyond this many are ignored, which is also all a browser tends to expose. */
const MAX_GAMEPADS = 4

/** Axis movement smaller than this is treated as the stick being at rest, since real sticks drift. */
const STICK_DEADZONE = 0.08

/** Modifier bits in the message's `m` field. */
const MODIFIER = { shift: 1, ctrl: 2, alt: 4, meta: 8 }

/** How many of Moonlight's 120ths of a wheel notch a browser pixel of scrolling is worth. */
const SCROLL_UNITS_PER_PIXEL = 1.2

/** Pixels a browser reports for one line of scrolling, when it reports lines instead. */
const PIXELS_PER_LINE = 40

/** Pixels a browser reports for one page of scrolling, when it reports pages instead. */
const PIXELS_PER_PAGE = 800

/**
 * Get the Windows virtual-key code for a physical key.
 *
 * @param {string} code The `KeyboardEvent.code` value.
 * @returns {number|null} The virtual-key code, or `null` for a key with no equivalent.
 */
export function vkForCode(code) {
  return VK_BY_CODE[code] ?? null
}

/**
 * Get the modifier bitmask for a keyboard or mouse event.
 *
 * @param {{shiftKey: boolean, ctrlKey: boolean, altKey: boolean, metaKey: boolean}} event The event.
 * @returns {number} Shift 1, Ctrl 2, Alt 4 and Meta 8, combined.
 */
export function modifierMask(event) {
  return (event.shiftKey ? MODIFIER.shift : 0) | (event.ctrlKey ? MODIFIER.ctrl : 0) | (event.altKey ? MODIFIER.alt : 0) | (event.metaKey ? MODIFIER.meta : 0)
}

/**
 * Map a DOM mouse button number to Moonlight's.
 *
 * @param {number} button The `MouseEvent.button` value: 0 left, 1 middle, 2 right, 3 back, 4 forward.
 * @returns {number|null} Moonlight's number (1 to 5), or `null` for a button it has no equivalent for.
 */
export function mouseButtonFor(button) {
  return button >= 0 && button <= 4 ? [1, 2, 3, 4, 5][button] : null
}

/**
 * Convert a wheel event into scroll amounts in Moonlight's units, 120 to a notch.
 *
 * The browser's vertical direction is the opposite of Moonlight's: scrolling down is positive in
 * the browser and negative to the host.
 *
 * @param {{deltaX: number, deltaY: number, deltaMode: number}} event The wheel event.
 * @returns {{vertical: number, horizontal: number}} The amounts, rounded to whole units.
 */
export function wheelToScroll(event) {
  const pixels = event.deltaMode === 1 ? PIXELS_PER_LINE : event.deltaMode === 2 ? PIXELS_PER_PAGE : 1
  return {
    vertical: -Math.round(event.deltaY * pixels * SCROLL_UNITS_PER_PIXEL) || 0,
    horizontal: Math.round(event.deltaX * pixels * SCROLL_UNITS_PER_PIXEL) || 0,
  }
}

/**
 * Work out where a pointer is within the picture the video actually shows.
 *
 * A video element is usually larger than its picture, which is centred with bars either side, so a
 * position has to be measured against the picture rather than the element.
 *
 * @param {{left: number, top: number, width: number, height: number}} box The element's bounding box.
 * @param {number} videoWidth Width of the video's picture in pixels, or 0 if it has none yet.
 * @param {number} videoHeight Height of the video's picture in pixels, or 0 if it has none yet.
 * @param {number} clientX Pointer x in the page.
 * @param {number} clientY Pointer y in the page.
 * @returns {{x: number, y: number, w: number, h: number}|null} The position within the picture and
 *   the picture's size in whole pixels, or `null` if the pointer is over the bars or there is no picture.
 */
export function positionInPicture(box, videoWidth, videoHeight, clientX, clientY) {
  if (!videoWidth || !videoHeight || !box.width || !box.height) {
    return null
  }

  const scale = Math.min(box.width / videoWidth, box.height / videoHeight)
  const width = videoWidth * scale
  const height = videoHeight * scale
  const left = box.left + (box.width - width) / 2
  const top = box.top + (box.height - height) / 2

  const x = clientX - left
  const y = clientY - top
  if (x < 0 || y < 0 || x > width || y > height) {
    return null
  }

  return { x: Math.round(x), y: Math.round(y), w: Math.max(1, Math.round(width)), h: Math.max(1, Math.round(height)) }
}

/**
 * Convert a stick or trigger reading to what Moonlight expects.
 *
 * @param {number} value Axis reading from -1 to 1.
 * @returns {number} The value scaled to a signed 16-bit range, or 0 inside the dead zone.
 */
function stickValue(value) {
  return Math.abs(value) < STICK_DEADZONE ? 0 : Math.max(-32768, Math.min(32767, Math.round(value * 32767)))
}

/**
 * Build the state message for a controller from the browser's view of it.
 *
 * @param {number} index Controller number.
 * @param {{buttons: Array<{pressed: boolean, value: number}>, axes: number[]}} pad The gamepad.
 * @returns {object} A `gp` message.
 */
export function gamepadStateMessage(index, pad) {
  let buttons = 0
  for (const [button, flag] of Object.entries(GAMEPAD_BUTTON_FLAGS)) {
    if (pad.buttons[button]?.pressed) {
      buttons |= flag
    }
  }

  const trigger = (button) => Math.max(0, Math.min(255, Math.round((pad.buttons[button]?.value ?? 0) * 255)))

  return {
    t: 'gp',
    n: index,
    b: buttons,
    lt: trigger(6),
    rt: trigger(7),
    lx: stickValue(pad.axes[0] ?? 0),
    // A browser's stick is negative when pushed up, and Moonlight's is positive.
    ly: -stickValue(pad.axes[1] ?? 0) || 0,
    rx: stickValue(pad.axes[2] ?? 0),
    ry: -stickValue(pad.axes[3] ?? 0) || 0,
  }
}

/**
 * Sends the browser's keyboard, mouse and gamepad input to the host.
 */
export class InputSender {
  #send
  #element
  #getGamepads
  #requestFrame
  #cancelFrame
  #started = false
  #relative = false
  #frame = null
  #pendingDelta = { dx: 0, dy: 0 }
  #pendingPosition = null
  #heldKeys = new Set()
  #heldButtons = new Set()
  #lastGamepads = new Map()
  #listeners = []

  /**
   * @param {object} options
   * @param {(message: string) => void} options.send Delivers one JSON message to the host.
   * @param {HTMLElement} options.element The element showing the stream, which pointer events are measured against.
   * @param {() => Array<Gamepad|null>} [options.getGamepads] Reads the connected controllers.
   * @param {(callback: () => void) => any} [options.requestFrame] Schedules work for the next frame.
   * @param {(handle: any) => void} [options.cancelFrame] Cancels work scheduled with requestFrame.
   */
  constructor({ send, element, getGamepads, requestFrame, cancelFrame }) {
    this.#send = send
    this.#element = element
    this.#getGamepads = getGamepads ?? (() => (typeof navigator !== 'undefined' && navigator.getGamepads ? Array.from(navigator.getGamepads()) : []))
    this.#requestFrame = requestFrame ?? ((callback) => requestAnimationFrame(callback))
    this.#cancelFrame = cancelFrame ?? ((handle) => cancelAnimationFrame(handle))
  }

  /**
   * Whether pointer movement is being sent as relative motion, as it should be while the pointer is locked.
   *
   * @returns {boolean}
   */
  get relative() {
    return this.#relative
  }

  /**
   * Choose between relative motion (for a locked pointer) and absolute positions.
   *
   * @param {boolean} relative True for relative motion.
   */
  setRelative(relative) {
    this.#flushMotion()
    this.#relative = relative
  }

  /**
   * Ask the browser to lock the pointer to the stream. This must be called from a user gesture.
   */
  lockPointer() {
    this.#element.requestPointerLock?.()
  }

  /**
   * Start listening for input.
   */
  start() {
    if (this.#started) {
      return
    }
    this.#started = true

    const doc = this.#element.ownerDocument
    const win = doc.defaultView

    this.#listen(win, 'keydown', (event) => this.#onKey(event, false))
    this.#listen(win, 'keyup', (event) => this.#onKey(event, true))
    this.#listen(win, 'blur', () => this.#releaseEverything())
    this.#listen(this.#element, 'mousemove', (event) => this.#onMouseMove(event))
    this.#listen(this.#element, 'mousedown', (event) => this.#onMouseButton(event, true))
    this.#listen(win, 'mouseup', (event) => this.#onMouseButton(event, false))
    this.#listen(this.#element, 'wheel', (event) => this.#onWheel(event), { passive: false })
    this.#listen(this.#element, 'contextmenu', (event) => event.preventDefault())
    this.#listen(doc, 'pointerlockchange', () => this.setRelative(doc.pointerLockElement === this.#element))

    this.#frame = this.#requestFrame(() => this.#onFrame())
  }

  /**
   * Stop listening, and release anything still held so nothing is left stuck down on the host.
   */
  stop() {
    if (!this.#started) {
      return
    }
    this.#started = false

    for (const [target, type, handler, options] of this.#listeners) {
      target.removeEventListener(type, handler, options)
    }
    this.#listeners = []

    if (this.#frame !== null) {
      this.#cancelFrame(this.#frame)
      this.#frame = null
    }

    this.#flushMotion()
    this.#releaseEverything()
    for (const index of this.#lastGamepads.keys()) {
      this.#emit({ t: 'gp', n: index, m: 0 })
    }
    this.#lastGamepads.clear()
  }

  #listen(target, type, handler, options) {
    target.addEventListener(type, handler, options)
    this.#listeners.push([target, type, handler, options])
  }

  #emit(message) {
    this.#send(JSON.stringify(message))
  }

  #onKey(event, release) {
    const vk = vkForCode(event.code)
    if (vk === null) {
      return
    }

    // The host repeats held keys itself, so the browser's own repeats would double them.
    if (!release && event.repeat) {
      event.preventDefault()
      return
    }

    // A release for a key that was never pressed here, such as one held when the stream started.
    if (release && !this.#heldKeys.has(event.code)) {
      return
    }

    event.preventDefault()
    this.#flushMotion()
    if (release) {
      this.#heldKeys.delete(event.code)
    } else {
      this.#heldKeys.add(event.code)
    }
    this.#emit({ t: release ? 'ku' : 'kd', k: vk, m: modifierMask(event) })
  }

  #onMouseMove(event) {
    if (this.#relative) {
      this.#pendingDelta.dx += event.movementX || 0
      this.#pendingDelta.dy += event.movementY || 0
      return
    }

    const box = this.#element.getBoundingClientRect()
    const position = positionInPicture(box, this.#element.videoWidth, this.#element.videoHeight, event.clientX, event.clientY)
    if (position) {
      this.#pendingPosition = position
    }
  }

  #onMouseButton(event, down) {
    const button = mouseButtonFor(event.button)
    if (button === null) {
      return
    }

    // Only presses that start on the stream count, but a release anywhere ends a held button.
    if (down) {
      this.#heldButtons.add(button)
    } else if (!this.#heldButtons.delete(button)) {
      return
    }

    event.preventDefault()
    this.#flushMotion()
    this.#emit({ t: 'mb', b: button, d: down })
  }

  #onWheel(event) {
    event.preventDefault()
    const { vertical, horizontal } = wheelToScroll(event)
    this.#flushMotion()
    if (vertical) {
      this.#emit({ t: 'sv', a: vertical })
    }
    if (horizontal) {
      this.#emit({ t: 'sh', a: horizontal })
    }
  }

  /**
   * Send any pending pointer movement now, so it is never reordered after a click or key.
   */
  #flushMotion() {
    if (this.#pendingDelta.dx || this.#pendingDelta.dy) {
      this.#emit({ t: 'mr', dx: this.#pendingDelta.dx, dy: this.#pendingDelta.dy })
      this.#pendingDelta = { dx: 0, dy: 0 }
    }
    if (this.#pendingPosition) {
      this.#emit({ t: 'ma', ...this.#pendingPosition })
      this.#pendingPosition = null
    }
  }

  #releaseEverything() {
    for (const code of this.#heldKeys) {
      this.#emit({ t: 'ku', k: vkForCode(code), m: 0 })
    }
    this.#heldKeys.clear()
    for (const button of this.#heldButtons) {
      this.#emit({ t: 'mb', b: button, d: false })
    }
    this.#heldButtons.clear()
  }

  #onFrame() {
    if (!this.#started) {
      return
    }

    // Movement is sent once a frame however many events the browser produced, which keeps a
    // high-rate mouse from flooding the channel.
    this.#flushMotion()
    this.#pollGamepads()
    this.#frame = this.#requestFrame(() => this.#onFrame())
  }

  #pollGamepads() {
    const seen = new Set()
    const pads = this.#getGamepads()

    for (let index = 0; index < Math.min(pads.length, MAX_GAMEPADS); index++) {
      const pad = pads[index]
      if (!pad || !pad.connected) {
        continue
      }
      seen.add(index)

      const message = gamepadStateMessage(index, pad)
      const signature = JSON.stringify(message)
      const previous = this.#lastGamepads.get(index)

      if (previous === undefined) {
        // Announce a controller before reporting on it, so the host knows what it is.
        this.#emit({ t: 'ga', n: index })
      }
      if (previous !== signature) {
        this.#emit(message)
        this.#lastGamepads.set(index, signature)
      }
    }

    // A controller that has gone is reported as removed, so the host does not keep a phantom one.
    for (const index of [...this.#lastGamepads.keys()]) {
      if (!seen.has(index)) {
        this.#emit({ t: 'gp', n: index, m: 0 })
        this.#lastGamepads.delete(index)
      }
    }
  }
}
