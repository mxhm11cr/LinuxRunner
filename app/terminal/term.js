const { init, Terminal } = window.ghosttyWeb;

const terminalElement = document.getElementById('terminal');
const ansiColorNames = [
    'black',
    'red',
    'green',
    'yellow',
    'blue',
    'magenta',
    'cyan',
    'white',
    'brightBlack',
    'brightRed',
    'brightGreen',
    'brightYellow',
    'brightBlue',
    'brightMagenta',
    'brightCyan',
    'brightWhite',
];

let term;
let styleState = {
    sharpness: 'sharp',
    foregroundColor: '#f0f0f0',
    backgroundColor: '#000000',
    cursorColor: undefined,
    fontFamily: '"JetBrainsMono Nerd Font Mono", "FiraCode Nerd Font Mono", ui-monospace, "SFMono-Regular", Menlo, Monaco, monospace',
    fontSize: 12,
    colorPaletteOverrides: undefined,
    blinkCursor: false,
    cursorShape: 'BLOCK',
};
styleState = {...styleState, ...normalizeStyleUpdate(window.__terminalInitialStyle)};
applyDocumentStyle(styleState);
let resizeObserver;
let pendingResizePreview = false;
let pendingResizeCommitTimeout;
let pendingScrollSync = false;
let lastNativeScrollHeight;
let lastNativeScrollTop;
let oldProps = {};
const resizeSettleDelayMs = 120;

// Functions for native -> JS.  Define this immediately so early native calls
// fail closed instead of throwing while the module/WASM is still loading.
window.exports = {
    write() {},
    getSize: () => [0, 0],
    copy: () => false,
    setFocused() {},
    scrollToBottom() {},
    newScrollTop() {},
    updateStyle(nextStyle) {
        styleState = {...styleState, ...normalizeStyleUpdate(nextStyle)};
        applyDocumentStyle(styleState);
    },
    getCharacterSize: () => [0, 0],
    clearScrollback() {},
    setUserGesture() {},
    setAccessibilityEnabled() {},
};

const native = new Proxy({}, {
    get(_obj, prop) {
        return (...args) => {
            if (!window.webkit?.messageHandlers?.[prop])
                return;
            let body = null;
            if (args.length == 1)
                body = args[0];
            else if (args.length > 1)
                body = args;
            webkit.messageHandlers[prop].postMessage(body);
        };
    },
});

window.addEventListener('error', (event) => {
    native.log(`terminal frontend error: ${event.message}`);
});
window.addEventListener('unhandledrejection', (event) => {
    native.log(`terminal frontend rejection: ${event.reason}`);
});

window.addEventListener('load', async () => {
    try {
        await loadConfiguredFont(styleState);

        await init();

        const terminalOptions = {
            cols: 80,
            rows: 24,
            allowTransparency: true,
            cursorBlink: styleState.blinkCursor,
            cursorStyle: cursorStyleForGhostty(styleState.cursorShape),
            devicePixelRatio: preferredDevicePixelRatio(),
            fontFamily: styleState.fontFamily,
            fontSize: styleState.fontSize,
            scrollbarWidth: 0,
            smoothScrollDuration: 0,
            theme: themeForGhostty(styleState),
        };
        term = new Terminal(terminalOptions);
        window.term = term;

        term.onData((data) => {
            native.sendInput(data);
            syncApplicationCursor();
        });
        term.onResize(() => {
            native.resize();
            scheduleScrollSync();
        });
        term.onScroll(scheduleScrollSync);
        term.onCursorMove(syncApplicationCursor);

        term.open(terminalElement);
        disableWebTextInput();
        installBridgeExports();
        installFocusBridge();
        installResizeBridge();
        fitTerminal();
        syncApplicationCursor();
        scheduleScrollSync();

        native.load();
        native.syncFocus();
    } catch (error) {
        native.log(`terminal frontend failed to load: ${error?.stack || error}`);
        throw error;
    }
});

function installBridgeExports() {
    window.exports.write = (data) => {
        const bytes = latin1StringToBytes(data);
        term.write(bytes);
        if (bytes.includes(0x1b))
            syncApplicationCursor();
        scheduleScrollSync();
    };

    window.exports.getSize = () => [term.cols, term.rows];

    window.exports.copy = () => term.copySelection();

    window.exports.setFocused = (focus) => {
        terminalElement.classList.toggle('terminal-focused', !!focus);
    };

    window.exports.scrollToBottom = () => {
        term.scrollToBottom();
        scheduleScrollSync();
    };

    window.exports.newScrollTop = (y) => {
        if (!Number.isFinite(y))
            return;
        const metrics = term.renderer?.getMetrics();
        const cellHeight = metrics?.height || 1;
        const scrollback = term.getScrollbackLength();
        const viewportY = scrollback - (y / cellHeight);
        term.scrollToLine(Math.round(viewportY));
        scheduleScrollSync();
    };

    window.exports.updateStyle = async (nextStyle) => {
        styleState = {...styleState, ...normalizeStyleUpdate(nextStyle)};
        applyDocumentStyle(styleState);
        await loadConfiguredFont(styleState);

        if (term.options.fontFamily !== styleState.fontFamily)
            term.options.fontFamily = styleState.fontFamily;
        if (term.options.fontSize !== styleState.fontSize)
            term.options.fontSize = styleState.fontSize;
        term.options.cursorBlink = !!styleState.blinkCursor;
        term.options.cursorStyle = cursorStyleForGhostty(styleState.cursorShape);
        term.options.theme = themeForGhostty(styleState);
        applyPixelRatio();

        fitTerminal();
        forceFullRepaint();
        scheduleScrollSync();
    };

    window.exports.getCharacterSize = () => {
        const metrics = term.renderer?.getMetrics();
        return [metrics?.width || 0, metrics?.height || 0];
    };

    window.exports.clearScrollback = () => {
        term.write('\x1b[3J');
        term.scrollToBottom();
        scheduleScrollSync();
    };

    window.exports.setUserGesture = () => {};
    window.exports.setAccessibilityEnabled = () => {};
}

function normalizeStyleUpdate(nextStyle) {
    const style = {...(nextStyle || {})};
    if (!Array.isArray(style.colorPaletteOverrides))
        style.colorPaletteOverrides = undefined;
    return style;
}

function applyDocumentStyle(style) {
    if (style.backgroundColor)
        document.documentElement.style.setProperty('--terminal-background', style.backgroundColor);
    if (style.foregroundColor)
        document.documentElement.style.setProperty('--terminal-foreground', style.foregroundColor);
}

function preferredDevicePixelRatio() {
    // The canvas backing store must match the screen's pixel density or text
    // is upscaled and blurry (a forced 1x on 3x iPhones was the blur users saw).
    // The renderer redraws only changed rows, so native density is the default;
    // "balanced" caps it at 2x and "battery" restores the old 1x.
    const native = window.devicePixelRatio || 1;
    switch (styleState.sharpness) {
        case 'battery': return 1;
        case 'balanced': return Math.min(2, native);
        default: return native;
    }
}

function applyPixelRatio() {
    const renderer = term?.renderer;
    if (!renderer)
        return;
    const ratio = preferredDevicePixelRatio();
    if (renderer.fixedDevicePixelRatio === ratio)
        return;
    renderer.fixedDevicePixelRatio = ratio;
    renderer.devicePixelRatio = ratio;
    if (renderer.resize)
        renderer.resize(term.cols, term.rows);
}

function installFocusBridge() {
    terminalElement.addEventListener('touchstart', (event) => {
        if (!term.hasSelection())
            event.preventDefault();
    }, {capture: true});
    terminalElement.addEventListener('touchend', (event) => {
        if (term.hasSelection())
            return;
        event.preventDefault();
        event.stopImmediatePropagation();
        native.focus();
    }, {capture: true});
    terminalElement.addEventListener('mousedown', (event) => {
        event.preventDefault();
        if (!term.hasSelection())
            native.focus();
    }, {capture: true});
    terminalElement.addEventListener('focus', () => native.syncFocus());
    terminalElement.addEventListener('blur', () => native.syncFocus());
}

function disableWebTextInput() {
    // iOS text entry is handled by TerminalView.insertText:.  Do not let the
    // WebView's hidden textarea become the keyboard owner, or native input
    // stops reaching the pty.
    terminalElement.removeAttribute('contenteditable');
    terminalElement.removeAttribute('role');
    terminalElement.removeAttribute('aria-label');
    terminalElement.removeAttribute('aria-multiline');
    terminalElement.setAttribute('tabindex', '-1');
    if (term.textarea) {
        term.textarea.readOnly = true;
        term.textarea.setAttribute('tabindex', '-1');
        term.textarea.style.pointerEvents = 'none';
        term.textarea.style.webkitUserSelect = 'none';
        term.textarea.style.userSelect = 'none';
        term.textarea.blur();
    }
}

function installResizeBridge() {
    resizeObserver = new ResizeObserver(() => scheduleFit());
    resizeObserver.observe(terminalElement);
    window.addEventListener('resize', scheduleFit);
}

function scheduleFit() {
    requestResizePreview();
    clearTimeout(pendingResizeCommitTimeout);
    pendingResizeCommitTimeout = setTimeout(fitTerminal, resizeSettleDelayMs);
}

function requestResizePreview() {
    if (pendingResizePreview)
        return;
    pendingResizePreview = true;
    requestAnimationFrame(() => {
        pendingResizePreview = false;
        previewTerminalSize();
    });
}

function previewTerminalSize() {
    if (!term?.renderer || !terminalElement)
        return;

    const canvas = term.renderer.getCanvas?.() || term.canvas;
    if (!canvas)
        return;

    const currentWidth = canvas.offsetWidth;
    const currentHeight = canvas.offsetHeight;
    const nextWidth = terminalElement.clientWidth;
    const nextHeight = terminalElement.clientHeight;
    if (currentWidth <= 0 || currentHeight <= 0 || nextWidth <= 0 || nextHeight <= 0)
        return;

    const scaleX = nextWidth / currentWidth;
    const scaleY = nextHeight / currentHeight;
    canvas.style.transformOrigin = 'top left';
    canvas.style.transform = `translateZ(0) scale(${scaleX}, ${scaleY})`;
}

function fitTerminal() {
    if (!term?.renderer || !terminalElement)
        return;

    const metrics = term.renderer.getMetrics();
    if (!metrics.width || !metrics.height)
        return;

    const width = terminalElement.clientWidth;
    const height = terminalElement.clientHeight;
    if (width <= 0 || height <= 0)
        return;

    const canvas = term.renderer.getCanvas?.() || term.canvas;
    if (canvas)
        canvas.style.transform = 'translateZ(0)';

    const cols = Math.max(2, Math.floor(width / metrics.width));
    const rows = Math.max(1, Math.floor(height / metrics.height));
    if (cols != term.cols || rows != term.rows) {
        term.resize(cols, rows);
        native.resize();
        scheduleScrollSync();
    }
}

function forceFullRepaint() {
    if (!term?.renderer || !term?.wasmTerm)
        return;

    term.renderer.render(term.wasmTerm, true, term.viewportY || 0, term, term.scrollbarOpacity || 0);
    term.requestRender?.();
}

function scheduleScrollSync() {
    if (pendingScrollSync)
        return;
    pendingScrollSync = true;
    requestAnimationFrame(() => {
        pendingScrollSync = false;
        syncScroll();
    });
}

function syncScroll() {
    if (!term?.renderer)
        return;

    const metrics = term.renderer.getMetrics();
    const scrollback = term.getScrollbackLength();
    const scrollHeight = (scrollback + term.rows) * metrics.height;
    const scrollTop = (scrollback - term.getViewportY()) * metrics.height;

    if (scrollHeight !== lastNativeScrollHeight) {
        native.newScrollHeight(scrollHeight);
        lastNativeScrollHeight = scrollHeight;
    }
    if (scrollTop !== lastNativeScrollTop) {
        native.newScrollTop(scrollTop);
        lastNativeScrollTop = scrollTop;
    }
}

function syncApplicationCursor() {
    if (!term)
        return;
    syncProp('applicationCursor', term.getMode(1, false));
}

function syncProp(name, value) {
    if (oldProps[name] !== value)
        native.propUpdate(name, value);
    oldProps[name] = value;
}

function latin1StringToBytes(data) {
    const bytes = new Uint8Array(data.length);
    for (let i = 0; i < data.length; i++)
        bytes[i] = data.charCodeAt(i) & 0xff;
    return bytes;
}

async function loadConfiguredFont(style) {
    if (!document.fonts?.load)
        return;
    try {
        await document.fonts.load(`${style.fontSize}px ${quoteFontFamily(style.fontFamily)}`);
    } catch (error) {
        native.log(`terminal font load failed: ${error}`);
    }
}

function quoteFontFamily(family) {
    return family.split(',')
        .map((part) => {
            const trimmed = part.trim();
            if (!trimmed.includes(' ') || trimmed.startsWith('"') || trimmed.startsWith("'"))
                return trimmed;
            return `"${trimmed}"`;
        })
        .join(', ');
}

function cursorStyleForGhostty(cursorShape) {
    switch (cursorShape) {
        case 'BEAM':
        case 'bar':
            return 'bar';
        case 'UNDERLINE':
        case 'underline':
            return 'underline';
        case 'BLOCK':
        case 'block':
        default:
            return 'block';
    }
}

function themeForGhostty(style) {
    const theme = {
        foreground: style.foregroundColor,
        background: style.backgroundColor,
        cursor: style.cursorColor || style.foregroundColor,
        cursorAccent: style.backgroundColor,
        selectionBackground: style.foregroundColor,
        selectionForeground: style.backgroundColor,
    };
    if (Array.isArray(style.colorPaletteOverrides)) {
        for (let i = 0; i < Math.min(ansiColorNames.length, style.colorPaletteOverrides.length); i++)
            theme[ansiColorNames[i]] = style.colorPaletteOverrides[i];
    }
    return theme;
}
