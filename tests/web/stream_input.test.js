import { afterEach, beforeEach, describe, expect, it } from 'vitest'

import {
  gamepadStateMessage,
  InputSender,
  modifierMask,
  mouseButtonFor,
  positionInPicture,
  vkForCode,
  wheelToScroll,
} from '../../src_assets/common/assets/web/stream/input.js'

describe('vkForCode', () => {
  it('maps letters and digits to their Windows codes', () => {
    expect(vkForCode('KeyA')).toBe(0x41)
    expect(vkForCode('KeyZ')).toBe(0x5a)
    expect(vkForCode('Digit0')).toBe(0x30)
    expect(vkForCode('Digit9')).toBe(0x39)
  })

  it('maps function keys and the numpad', () => {
    expect(vkForCode('F1')).toBe(0x70)
    expect(vkForCode('F12')).toBe(0x7b)
    expect(vkForCode('F24')).toBe(0x87)
    expect(vkForCode('Numpad0')).toBe(0x60)
    expect(vkForCode('Numpad9')).toBe(0x69)
    expect(vkForCode('NumpadAdd')).toBe(0x6b)
  })

  it('distinguishes the left and right modifier keys', () => {
    expect(vkForCode('ShiftLeft')).toBe(0xa0)
    expect(vkForCode('ShiftRight')).toBe(0xa1)
    expect(vkForCode('ControlLeft')).toBe(0xa2)
    expect(vkForCode('AltRight')).toBe(0xa5)
    expect(vkForCode('MetaLeft')).toBe(0x5b)
  })

  it('maps navigation, editing and punctuation keys', () => {
    expect(vkForCode('Escape')).toBe(0x1b)
    expect(vkForCode('Enter')).toBe(0x0d)
    expect(vkForCode('NumpadEnter')).toBe(0x0d)
    expect(vkForCode('ArrowLeft')).toBe(0x25)
    expect(vkForCode('ArrowDown')).toBe(0x28)
    expect(vkForCode('Delete')).toBe(0x2e)
    expect(vkForCode('Semicolon')).toBe(0xba)
    expect(vkForCode('BracketRight')).toBe(0xdd)
  })

  it('gives a distinct code to every key it knows', () => {
    const codes = ['KeyA', 'KeyB', 'Digit1', 'F1', 'Numpad1', 'Space', 'Tab', 'ArrowUp', 'Backquote']
    expect(new Set(codes.map(vkForCode)).size).toBe(codes.length)
  })

  it('returns null for keys with no equivalent', () => {
    expect(vkForCode('MediaPlayPause')).toBeNull()
    expect(vkForCode('')).toBeNull()
    expect(vkForCode(undefined)).toBeNull()
  })

  it('stays within what the host accepts for a key code', () => {
    for (const code of ['KeyA', 'F24', 'IntlBackslash', 'AltRight', 'NumLock']) {
      expect(vkForCode(code)).toBeGreaterThanOrEqual(1)
      expect(vkForCode(code)).toBeLessThanOrEqual(254)
    }
  })
})

describe('modifierMask', () => {
  it('combines the modifier keys into a bitmask', () => {
    expect(modifierMask({})).toBe(0)
    expect(modifierMask({ shiftKey: true })).toBe(1)
    expect(modifierMask({ ctrlKey: true })).toBe(2)
    expect(modifierMask({ altKey: true })).toBe(4)
    expect(modifierMask({ metaKey: true })).toBe(8)
    expect(modifierMask({ shiftKey: true, ctrlKey: true, altKey: true, metaKey: true })).toBe(15)
  })
})

describe('mouseButtonFor', () => {
  it('maps DOM buttons to Moonlight buttons', () => {
    expect([0, 1, 2, 3, 4].map(mouseButtonFor)).toEqual([1, 2, 3, 4, 5])
  })

  it('rejects buttons Moonlight does not have', () => {
    expect(mouseButtonFor(5)).toBeNull()
    expect(mouseButtonFor(-1)).toBeNull()
  })
})

describe('wheelToScroll', () => {
  it('turns a notch of scrolling into 120 units, downwards being negative to the host', () => {
    expect(wheelToScroll({ deltaX: 0, deltaY: 100, deltaMode: 0 })).toEqual({ vertical: -120, horizontal: 0 })
    expect(wheelToScroll({ deltaX: 0, deltaY: -100, deltaMode: 0 })).toEqual({ vertical: 120, horizontal: 0 })
  })

  it('keeps horizontal scrolling in the browser direction', () => {
    expect(wheelToScroll({ deltaX: 100, deltaY: 0, deltaMode: 0 })).toEqual({ vertical: 0, horizontal: 120 })
  })

  it('understands line and page units', () => {
    expect(wheelToScroll({ deltaX: 0, deltaY: 1, deltaMode: 1 }).vertical).toBe(-48)
    expect(wheelToScroll({ deltaX: 0, deltaY: 1, deltaMode: 2 }).vertical).toBe(-960)
  })

  it('reports zero rather than negative zero', () => {
    const result = wheelToScroll({ deltaX: 0, deltaY: 0, deltaMode: 0 })
    expect(Object.is(result.vertical, 0)).toBe(true)
    expect(Object.is(result.horizontal, 0)).toBe(true)
  })
})

describe('positionInPicture', () => {
  const box = { left: 10, top: 20, width: 1000, height: 600 }

  it('measures against the picture, not the element, when the picture is letterboxed', () => {
    // A 1600x900 picture fits 1000 wide by 562.5 tall, centred with bars above and below.
    const centre = positionInPicture(box, 1600, 900, 510, 320)
    expect(centre).toEqual({ x: 500, y: 281, w: 1000, h: 563 })
  })

  it('measures against the picture when it is pillarboxed', () => {
    // A 600x600 picture fits 600 wide, centred with bars to the left and right.
    const corner = positionInPicture(box, 600, 600, 210, 20)
    expect(corner).toEqual({ x: 0, y: 0, w: 600, h: 600 })
  })

  it('ignores the pointer over the bars', () => {
    expect(positionInPicture(box, 600, 600, 100, 300)).toBeNull()
    expect(positionInPicture(box, 1600, 900, 500, 22)).toBeNull()
  })

  it('has nothing to say before there is a picture', () => {
    expect(positionInPicture(box, 0, 0, 500, 300)).toBeNull()
    expect(positionInPicture({ left: 0, top: 0, width: 0, height: 0 }, 1600, 900, 0, 0)).toBeNull()
  })

  it('never reports an empty picture size', () => {
    // A very wide picture in a 1x1 box is 1 pixel wide and a fraction of a pixel tall, which rounds to 0.
    const tiny = positionInPicture({ left: 0, top: 0, width: 1, height: 1 }, 3200, 900, 0.5, 0.5)
    expect(tiny.w).toBe(1)
    expect(tiny.h).toBe(1)
  })
})

describe('gamepadStateMessage', () => {
  const pad = (overrides = {}) => ({
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })),
    axes: [0, 0, 0, 0],
    ...overrides,
  })

  it('reports an idle controller as all zero', () => {
    expect(gamepadStateMessage(0, pad())).toEqual({ t: 'gp', n: 0, b: 0, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0 })
  })

  it('maps face buttons, bumpers, menu buttons and stick clicks', () => {
    const buttons = pad().buttons
    for (const index of [0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 16]) {
      buttons[index] = { pressed: true, value: 1 }
    }
    expect(gamepadStateMessage(0, pad({ buttons })).b).toBe(0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x0100 | 0x0200 | 0x0020 | 0x0010 | 0x0040 | 0x0080 | 0x0400)
  })

  it('maps the d-pad', () => {
    const buttons = pad().buttons
    for (const index of [12, 13, 14, 15]) {
      buttons[index] = { pressed: true, value: 1 }
    }
    expect(gamepadStateMessage(0, pad({ buttons })).b).toBe(0x0001 | 0x0002 | 0x0004 | 0x0008)
  })

  it('reports the analogue triggers by how far they are pulled', () => {
    const buttons = pad().buttons
    buttons[6] = { pressed: true, value: 1 }
    buttons[7] = { pressed: false, value: 0.5 }
    const message = gamepadStateMessage(2, pad({ buttons }))
    expect(message.n).toBe(2)
    expect(message.lt).toBe(255)
    expect(message.rt).toBe(128)
    // The triggers are not buttons as far as the button flags are concerned.
    expect(message.b).toBe(0)
  })

  it('scales the sticks to 16 bits and flips vertical so up is positive', () => {
    const message = gamepadStateMessage(0, pad({ axes: [1, -1, -1, 1] }))
    expect(message.lx).toBe(32767)
    expect(message.ly).toBe(32767)
    expect(message.rx).toBe(-32767)
    expect(message.ry).toBe(-32767)
  })

  it('treats small stick movement as rest, so drift is not sent', () => {
    const message = gamepadStateMessage(0, pad({ axes: [0.05, -0.07, 0.079, 0] }))
    expect(message.lx).toBe(0)
    expect(message.ly).toBe(0)
    expect(message.rx).toBe(0)
    expect(Object.is(message.ly, 0)).toBe(true)
  })

  it('copes with a controller that reports fewer buttons and axes than usual', () => {
    const message = gamepadStateMessage(0, { buttons: [], axes: [] })
    expect(message).toEqual({ t: 'gp', n: 0, b: 0, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0 })
  })
})

describe('InputSender', () => {
  let sent
  let frames
  let cancelled
  let element
  let gamepads
  let sender

  const messages = () => sent.map((text) => JSON.parse(text))
  const runFrame = () => {
    const callbacks = frames.splice(0)
    callbacks.forEach((callback) => callback())
  }
  const key = (type, code, init = {}) => {
    const event = new KeyboardEvent(type, { code, bubbles: true, cancelable: true, ...init })
    window.dispatchEvent(event)
    return event
  }
  const mouse = (type, init = {}, target = element) => {
    const event = new MouseEvent(type, { bubbles: true, cancelable: true, ...init })
    target.dispatchEvent(event)
    return event
  }

  beforeEach(() => {
    sent = []
    frames = []
    cancelled = []
    gamepads = []

    element = document.createElement('video')
    document.body.append(element)
    // jsdom has no layout or decoded video, so give the element a known size and picture.
    element.getBoundingClientRect = () => ({ left: 0, top: 0, width: 1000, height: 500 })
    Object.defineProperty(element, 'videoWidth', { value: 1000, configurable: true })
    Object.defineProperty(element, 'videoHeight', { value: 500, configurable: true })

    sender = new InputSender({
      send: (text) => sent.push(text),
      element,
      getGamepads: () => gamepads,
      requestFrame: (callback) => {
        frames.push(callback)
        return frames.length
      },
      cancelFrame: (handle) => cancelled.push(handle),
    })
    sender.start()
  })

  afterEach(() => {
    sender.stop()
    element.remove()
  })

  it('sends key presses and releases with the modifiers held', () => {
    key('keydown', 'KeyA', { shiftKey: true })
    key('keyup', 'KeyA', { shiftKey: true })
    expect(messages()).toEqual([
      { t: 'kd', k: 0x41, m: 1 },
      { t: 'ku', k: 0x41, m: 1 },
    ])
  })

  it('takes over the keys it handles, so the page does not also react to them', () => {
    expect(key('keydown', 'KeyA').defaultPrevented).toBe(true)
    expect(key('keyup', 'KeyA').defaultPrevented).toBe(true)
  })

  it('leaves keys it has no mapping for alone', () => {
    const event = key('keydown', 'MediaPlayPause')
    expect(event.defaultPrevented).toBe(false)
    expect(sent).toEqual([])
  })

  it('ignores the browser repeating a held key, since the host repeats it itself', () => {
    key('keydown', 'KeyA')
    const repeat = key('keydown', 'KeyA', { repeat: true })
    expect(repeat.defaultPrevented).toBe(true)
    expect(messages()).toEqual([{ t: 'kd', k: 0x41, m: 0 }])
  })

  it('ignores the release of a key that was never pressed on the stream', () => {
    key('keyup', 'KeyA')
    expect(sent).toEqual([])
  })

  it('releases held keys and buttons when the window loses focus, so none stay stuck down', () => {
    key('keydown', 'KeyA')
    key('keydown', 'ControlLeft', { ctrlKey: true })
    mouse('mousedown', { button: 0 })
    sent.length = 0

    window.dispatchEvent(new Event('blur'))
    expect(messages()).toEqual([
      { t: 'ku', k: 0x41, m: 0 },
      { t: 'ku', k: 0xa2, m: 0 },
      { t: 'mb', b: 1, d: false },
    ])

    // And it does not release them a second time.
    window.dispatchEvent(new Event('blur'))
    expect(messages()).toHaveLength(3)
  })

  it('sends absolute mouse positions measured against the picture, once per frame', () => {
    mouse('mousemove', { clientX: 100, clientY: 50 })
    mouse('mousemove', { clientX: 200, clientY: 100 })
    mouse('mousemove', { clientX: 300, clientY: 150 })
    expect(sent).toEqual([])

    runFrame()
    expect(messages()).toEqual([{ t: 'ma', x: 300, y: 150, w: 1000, h: 500 }])
  })

  it('sends nothing for a frame in which the mouse did not move', () => {
    runFrame()
    runFrame()
    expect(sent).toEqual([])
  })

  it('keeps a click after the movement that led to it', () => {
    mouse('mousemove', { clientX: 400, clientY: 250 })
    mouse('mousedown', { button: 0 })
    mouse('mouseup', { button: 0 }, window)
    expect(messages()).toEqual([
      { t: 'ma', x: 400, y: 250, w: 1000, h: 500 },
      { t: 'mb', b: 1, d: true },
      { t: 'mb', b: 1, d: false },
    ])
  })

  it('maps all five mouse buttons', () => {
    for (const button of [0, 1, 2, 3, 4]) {
      mouse('mousedown', { button })
      mouse('mouseup', { button }, window)
    }
    expect(messages().filter((m) => m.d).map((m) => m.b)).toEqual([1, 2, 3, 4, 5])
  })

  it('ignores a release when no button was pressed here, and buttons Moonlight lacks', () => {
    mouse('mouseup', { button: 0 }, window)
    mouse('mousedown', { button: 7 })
    expect(sent).toEqual([])
  })

  it('accumulates relative movement into a single message per frame', () => {
    sender.setRelative(true)
    mouse('mousemove', { movementX: 3, movementY: -2 })
    mouse('mousemove', { movementX: 4, movementY: 1 })
    runFrame()
    expect(messages()).toEqual([{ t: 'mr', dx: 7, dy: -1 }])
  })

  it('falls back to the reliable channel for motion when no separate one is given', () => {
    // The shared `sender` fixture only supplies `send`, so `sendMotion` defaults to it.
    mouse('mousemove', { clientX: 10, clientY: 10 })
    runFrame()
    expect(messages()).toEqual([{ t: 'ma', x: 10, y: 10, w: 1000, h: 500 }])
  })

  it('sends mouse motion on a separate channel when one is given, and everything else stays on the main one', () => {
    // A separate element and separate frame/cancel arrays, so this sender's own listeners are the only
    // ones reacting to what it dispatches - the shared `sender` from beforeEach listens on `element` too.
    const isolated = document.createElement('video')
    document.body.append(isolated)
    isolated.getBoundingClientRect = () => ({ left: 0, top: 0, width: 1000, height: 500 })
    Object.defineProperty(isolated, 'videoWidth', { value: 1000 })
    Object.defineProperty(isolated, 'videoHeight', { value: 500 })

    const main = []
    const motion = []
    const isolatedFrames = []
    const withMotion = new InputSender({
      send: (text) => main.push(JSON.parse(text)),
      sendMotion: (text) => motion.push(JSON.parse(text)),
      element: isolated,
      requestFrame: (callback) => {
        isolatedFrames.push(callback)
        return isolatedFrames.length
      },
      cancelFrame: () => {},
    })
    withMotion.start()

    mouse('mousemove', { clientX: 400, clientY: 250 }, isolated)
    mouse('mousedown', { button: 0 }, isolated)
    mouse('mouseup', { button: 0 }, window)
    isolatedFrames.splice(0).forEach((callback) => callback())

    expect(motion).toEqual([{ t: 'ma', x: 400, y: 250, w: 1000, h: 500 }])
    expect(main).toEqual([
      { t: 'mb', b: 1, d: true },
      { t: 'mb', b: 1, d: false },
    ])

    withMotion.stop()
    isolated.remove()
  })

  it('sends relative motion on the separate channel too', () => {
    const isolated = document.createElement('video')
    document.body.append(isolated)
    isolated.getBoundingClientRect = () => ({ left: 0, top: 0, width: 1000, height: 500 })

    const motion = []
    const isolatedFrames = []
    const withMotion = new InputSender({
      send: () => {},
      sendMotion: (text) => motion.push(JSON.parse(text)),
      element: isolated,
      requestFrame: (callback) => {
        isolatedFrames.push(callback)
        return isolatedFrames.length
      },
      cancelFrame: () => {},
    })
    withMotion.start()
    withMotion.setRelative(true)

    isolated.dispatchEvent(new MouseEvent('mousemove', { movementX: 3, movementY: -2, bubbles: true }))
    isolatedFrames.splice(0).forEach((callback) => callback())

    expect(motion).toEqual([{ t: 'mr', dx: 3, dy: -2 }])

    withMotion.stop()
    isolated.remove()
  })

  it('sends pending movement before switching between relative and absolute', () => {
    sender.setRelative(true)
    mouse('mousemove', { movementX: 5, movementY: 5 })
    sender.setRelative(false)
    expect(messages()).toEqual([{ t: 'mr', dx: 5, dy: 5 }])
  })

  it('ignores the pointer over the bars around the picture', () => {
    // The element is 1000x500 but the picture is square, so it is centred with bars either side.
    Object.defineProperty(element, 'videoWidth', { value: 500 })
    Object.defineProperty(element, 'videoHeight', { value: 500 })
    mouse('mousemove', { clientX: 10, clientY: 250 })
    runFrame()
    expect(sent).toEqual([])
  })

  it('reports the raw viewport position immediately, without waiting for a frame', () => {
    const isolated = document.createElement('video')
    document.body.append(isolated)
    isolated.getBoundingClientRect = () => ({ left: 0, top: 0, width: 1000, height: 500 })
    Object.defineProperty(isolated, 'videoWidth', { value: 1000 })
    Object.defineProperty(isolated, 'videoHeight', { value: 500 })

    const positions = []
    const withCallback = new InputSender({
      send: () => {},
      onAbsoluteMove: (position) => positions.push(position),
      element: isolated,
      requestFrame: () => 1,
      cancelFrame: () => {},
    })
    withCallback.start()

    isolated.dispatchEvent(new MouseEvent('mousemove', { clientX: 250, clientY: 125, bubbles: true }))
    // No frame was run, yet the position is already there.
    expect(positions).toEqual([{ clientX: 250, clientY: 125 }])

    isolated.dispatchEvent(new Event('mouseleave', { bubbles: true }))
    expect(positions.at(-1)).toBeNull()

    withCallback.stop()
    isolated.remove()
  })

  it('clears the absolute position when switching to relative movement', () => {
    const isolated = document.createElement('video')
    document.body.append(isolated)
    isolated.getBoundingClientRect = () => ({ left: 0, top: 0, width: 1000, height: 500 })
    Object.defineProperty(isolated, 'videoWidth', { value: 1000 })
    Object.defineProperty(isolated, 'videoHeight', { value: 500 })

    const positions = []
    const withCallback = new InputSender({
      send: () => {},
      onAbsoluteMove: (position) => positions.push(position),
      element: isolated,
      requestFrame: () => 1,
      cancelFrame: () => {},
    })
    withCallback.start()

    isolated.dispatchEvent(new MouseEvent('mousemove', { clientX: 100, clientY: 100, bubbles: true }))
    withCallback.setRelative(true)
    expect(positions.at(-1)).toBeNull()

    withCallback.stop()
    isolated.remove()
  })

  it('clears the absolute position when stopped', () => {
    const isolated = document.createElement('video')
    document.body.append(isolated)
    isolated.getBoundingClientRect = () => ({ left: 0, top: 0, width: 1000, height: 500 })
    Object.defineProperty(isolated, 'videoWidth', { value: 1000 })
    Object.defineProperty(isolated, 'videoHeight', { value: 500 })

    const positions = []
    const withCallback = new InputSender({
      send: () => {},
      onAbsoluteMove: (position) => positions.push(position),
      element: isolated,
      requestFrame: () => 1,
      cancelFrame: () => {},
    })
    withCallback.start()
    isolated.dispatchEvent(new MouseEvent('mousemove', { clientX: 5, clientY: 5, bubbles: true }))

    withCallback.stop()
    expect(positions.at(-1)).toBeNull()
    isolated.remove()
  })

  it('switches to relative movement when the pointer is locked, and back when it is released', () => {
    expect(sender.relative).toBe(false)
    Object.defineProperty(document, 'pointerLockElement', { value: element, configurable: true })
    document.dispatchEvent(new Event('pointerlockchange'))
    expect(sender.relative).toBe(true)

    Object.defineProperty(document, 'pointerLockElement', { value: null, configurable: true })
    document.dispatchEvent(new Event('pointerlockchange'))
    expect(sender.relative).toBe(false)
  })

  it('asks the browser to lock the pointer', () => {
    let requested = 0
    element.requestPointerLock = () => requested++
    sender.lockPointer()
    expect(requested).toBe(1)
  })

  it('copes with a browser that cannot lock the pointer', () => {
    element.requestPointerLock = undefined
    expect(() => sender.lockPointer()).not.toThrow()
  })

  it('sends vertical and horizontal scrolling, and stops the page scrolling too', () => {
    const wheel = new WheelEvent('wheel', { deltaX: 100, deltaY: 100, bubbles: true, cancelable: true })
    element.dispatchEvent(wheel)
    expect(wheel.defaultPrevented).toBe(true)
    expect(messages()).toEqual([
      { t: 'sv', a: -120 },
      { t: 'sh', a: 120 },
    ])
  })

  it('sends only the axis that scrolled', () => {
    element.dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }))
    expect(messages()).toEqual([{ t: 'sv', a: 120 }])
  })

  it('suppresses the context menu, so a right click reaches the host', () => {
    expect(mouse('contextmenu').defaultPrevented).toBe(true)
  })

  it('announces a controller, reports its state, and then only reports changes', () => {
    const pad = { connected: true, buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] }
    gamepads = [pad]

    runFrame()
    expect(messages()).toEqual([
      { t: 'ga', n: 0 },
      { t: 'gp', n: 0, b: 0, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0 },
    ])

    // Nothing changed, so nothing is sent.
    sent.length = 0
    runFrame()
    expect(sent).toEqual([])

    pad.buttons[0] = { pressed: true, value: 1 }
    runFrame()
    expect(messages()).toEqual([{ t: 'gp', n: 0, b: 0x1000, lt: 0, rt: 0, lx: 0, ly: 0, rx: 0, ry: 0 }])
  })

  it('reports a controller that goes away as removed, and announces it again if it returns', () => {
    const pad = { connected: true, buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] }
    gamepads = [pad]
    runFrame()
    sent.length = 0

    gamepads = [null]
    runFrame()
    expect(messages()).toEqual([{ t: 'gp', n: 0, m: 0 }])

    sent.length = 0
    gamepads = [pad]
    runFrame()
    expect(messages()[0]).toEqual({ t: 'ga', n: 0 })
  })

  it('treats a disconnected controller as absent, and ignores any beyond the fourth', () => {
    const idle = () => ({ connected: true, buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] })
    gamepads = [{ ...idle(), connected: false }, idle(), null, idle(), idle()]
    runFrame()
    expect(messages().filter((m) => m.t === 'ga').map((m) => m.n)).toEqual([1, 3])
  })

  it('tracks several controllers independently', () => {
    const idle = () => ({ connected: true, buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] })
    const first = idle()
    const second = idle()
    gamepads = [first, second]
    runFrame()
    sent.length = 0

    second.axes = [1, 0, 0, 0]
    runFrame()
    expect(messages()).toEqual([{ t: 'gp', n: 1, b: 0, lt: 0, rt: 0, lx: 32767, ly: 0, rx: 0, ry: 0 }])
  })

  it('keeps polling frame after frame while running', () => {
    runFrame()
    expect(frames).toHaveLength(1)
    runFrame()
    expect(frames).toHaveLength(1)
  })

  it('does not double up if started twice', () => {
    sender.start()
    key('keydown', 'KeyA')
    expect(messages()).toHaveLength(1)
  })

  it('stops listening, releases everything and cancels its frame when stopped', () => {
    key('keydown', 'KeyA')
    mouse('mousedown', { button: 2 })
    mouse('mousemove', { clientX: 10, clientY: 10 })
    gamepads = [{ connected: true, buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })), axes: [0, 0, 0, 0] }]
    runFrame()
    sent.length = 0

    sender.stop()
    expect(cancelled).toHaveLength(1)
    const stopped = messages()
    expect(stopped).toContainEqual({ t: 'ku', k: 0x41, m: 0 })
    expect(stopped).toContainEqual({ t: 'mb', b: 3, d: false })
    expect(stopped).toContainEqual({ t: 'gp', n: 0, m: 0 })

    // Nothing is heard after that.
    sent.length = 0
    key('keydown', 'KeyB')
    mouse('mousedown', { button: 0 })
    runFrame()
    expect(sent).toEqual([])
  })

  it('can be stopped when it was never started, or stopped twice', () => {
    const idle = new InputSender({ send: () => {}, element })
    expect(() => idle.stop()).not.toThrow()
    sender.stop()
    expect(() => sender.stop()).not.toThrow()
  })

  it('falls back to the browser for gamepads and frames when none are supplied', () => {
    const real = new InputSender({ send: () => {}, element })
    expect(() => real.start()).not.toThrow()
    real.stop()
  })
})
