/**
 * xterm-term-bridge.js — xterm.js terminal bridge for ios-linuxkit.
 *
 * Drop-in alternative to term.js (Ghostty). Uses the same native bridge API
 * (window.exports / webkit.messageHandlers) so the Obj-C host code doesn't
 * need changes — only the HTML entry point differs (xterm-term.html vs term.html).
 *
 * Build switch: in Terminal.m, load xterm-term.html instead of term.html to use
 * the xterm.js renderer instead of Ghostty WebGL/Canvas2D.
 */

(async () => {
    'use strict';

    window.exports = {
        write() {},
        getSize: () => [0, 0],
        copy: () => false,
        setFocused() {},
        scrollToBottom() {},
        newScrollTop() {},
        updateStyle() {},
        getCharacterSize: () => [0, 0],
        clearScrollback() {},
        setUserGesture() {},
        setAccessibilityEnabled() {},
    };

    const terminalElement = document.getElementById('terminal');
    const { Terminal, FitAddon, WebLinksAddon, LigaturesAddon } = window.xtermModules || {};
    const { CanvasAddon } = window.CanvasAddon || {};
    if (!Terminal || !FitAddon || !WebLinksAddon || !LigaturesAddon) {
        window.__terminalBootstrapLog?.('terminal xterm modules were not loaded');
        return;
    }
    if (!CanvasAddon) {
        window.__terminalBootstrapLog?.('terminal xterm canvas addon was not loaded');
        return;
    }

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

    // ── Native bridge (same as term.js) ──────────────────────────────────────
    const native = new Proxy({}, {
        get(_, prop) {
            return (...args) => {
                if (!window.webkit?.messageHandlers?.[prop])
                    return;
                let body = null;
                if (args.length == 1)
                    body = args[0];
                else if (args.length > 1)
                    body = args;
                window.webkit.messageHandlers[prop].postMessage(body);
            };
        }
    });

    // ── Style state ──────────────────────────────────────────────────────────
    let styleState = {
        foregroundColor: '#d4d4d4',
        backgroundColor: '#1e1e1e',
        cursorColor: undefined,
        fontSize: 12,
        fontFamily: '"JetBrainsMono Nerd Font Mono", "FiraCode Nerd Font Mono", ui-monospace, "SFMono-Regular", Menlo, Monaco, monospace',
        colorPaletteOverrides: undefined,
        blinkCursor: false,
        cursorShape: 'BLOCK',
    };
    styleState = {...styleState, ...normalizeStyleUpdate(window.__terminalInitialStyle)};
    applyDocumentStyle(styleState);
    await loadConfiguredFont(styleState);

    let resizeFrame = null;
    let pendingScrollSync = false;
    let lastNativeScrollHeight;
    let lastNativeScrollTop;
    let nativeScrollRequest = null;
    let oldProps = {};
    let xtermAppActive = null;
    let selectionAnchor = null;

    // ── Terminal setup ────────────────────────────────────────────────────────
    const term = new Terminal({
        cols: 80,
        rows: 24,
        fontSize: styleState.fontSize,
        fontFamily: styleState.fontFamily,
        theme: {
            foreground: styleState.foregroundColor,
            background: styleState.backgroundColor,
            cursor: styleState.cursorColor || styleState.foregroundColor,
            cursorAccent: styleState.backgroundColor,
        },
        cursorBlink: styleState.blinkCursor,
        cursorStyle: cursorStyleForXterm(styleState.cursorShape),
        allowTransparency: true,
        scrollback: 10000,
        convertEol: false,
        disableStdin: true,  // iOS handles text input natively
        allowProposedApi: true,
        overviewRuler: {width: 0},
        smoothScrollDuration: 0,
        windowsMode: false,
    });

    const fitAddon = new FitAddon();
    term.loadAddon(new CanvasAddon());
    term.loadAddon(fitAddon);
    term.loadAddon(new WebLinksAddon());

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
    term.onSelectionChange(() => native.selectionChanged(term.getSelection()));

    term.open(terminalElement);
    installNativeFocusAdapter();
    installLigatures();
    disableWebTextInput();
    installFocusBridge();
    fitAddon.fit();
    syncApplicationCursor();
    scheduleScrollSync();

    // ── Resize handling ──────────────────────────────────────────────────────
    function fitTerminal() {
        if (resizeFrame !== null)
            return;
        resizeFrame = requestAnimationFrame(() => {
            resizeFrame = null;
            fitAddon.fit();
            // The glyph atlas is built for the pixel density seen when the
            // terminal opened (1x while the web view was still offscreen);
            // WKWebView sends no DPR change, so rebuild it when the density
            // differs from the one the atlas was drawn at.
            if (window.devicePixelRatio !== fitTerminal.atlasRatio) {
                fitTerminal.atlasRatio = window.devicePixelRatio;
                term.clearTextureAtlas?.();
                term.refresh(0, term.rows - 1);
            }
            native.resize();
            scheduleScrollSync();
        });
    }

    const ro = new ResizeObserver(() => fitTerminal());
    ro.observe(terminalElement);

    // ── Exports (native → JS) ────────────────────────────────────────────────
    function latin1StringToBytes(str) {
        const bytes = new Uint8Array(str.length);
        for (let i = 0; i < str.length; i++) {
            bytes[i] = str.charCodeAt(i) & 0xff;
        }
        return bytes;
    }

    window.exports = {
        write(data) {
            const bytes = typeof data === 'string' ? latin1StringToBytes(data) : new Uint8Array(data);
            term.write(bytes, scheduleScrollSync);
            if (bytes.includes(0x1b))
                syncApplicationCursor();
        },
        getSize() {
            return [term.cols, term.rows];
        },
        copy() {
            return term.getSelection();
        },
        beginSelection(x, y) {
            selectionAnchor = selectionCell(x, y);
            if (selectionAnchor)
                term.select(selectionAnchor.col, selectionAnchor.row, 1);
        },
        extendSelection(x, y) {
            const end = selectionCell(x, y);
            if (!selectionAnchor || !end)
                return;
            const a = selectionAnchor.row * term.cols + selectionAnchor.col;
            const b = end.row * term.cols + end.col;
            const start = Math.min(a, b);
            term.select(start % term.cols, Math.floor(start / term.cols), Math.abs(a - b) + 1);
        },
        selectAll() { term.selectAll(); },
        clearSelection() { selectionAnchor = null; term.clearSelection(); },
        setFocused(focused) {
            terminalElement.classList.toggle('terminal-focused', !!focused);
            setXtermAppActive(!!focused);
        },
        scrollToBottom() {
            term.scrollToBottom();
            scheduleScrollSync();
        },
        newScrollTop(top) {
            if (!Number.isFinite(top))
                return;
            const cellHeight = getCellSize().height;
            if (!cellHeight)
                return;
            const buffer = term.buffer.active;
            const line = Math.max(0, Math.min(buffer.baseY, Math.round(top / cellHeight)));
            // UIKit owns fractional offsets and momentum. Do not snap its
            // offset back to the rounded row when acknowledging this request.
            nativeScrollRequest = {buffer, line, cellHeight};
            term.scrollToLine(line);
            scheduleScrollSync();
        },
        async updateStyle(newStyle) {
            styleState = {...styleState, ...normalizeStyleUpdate(newStyle)};
            applyDocumentStyle(styleState);
            await loadConfiguredFont(styleState);

            if (term.options.fontSize !== styleState.fontSize)
                term.options.fontSize = styleState.fontSize;
            if (term.options.fontFamily !== styleState.fontFamily)
                term.options.fontFamily = styleState.fontFamily;
            term.options.cursorBlink = !!styleState.blinkCursor;
            term.options.cursorStyle = cursorStyleForXterm(styleState.cursorShape);
            term.options.theme = themeForXterm(styleState);

            fitTerminal();
            term.refresh(0, term.rows - 1);
            scheduleScrollSync();
        },
        getCharacterSize() {
            const size = getCellSize();
            return [size.width || 0, size.height || 0];
        },
        clearScrollback() {
            term.clear();
            term.scrollToBottom();
            scheduleScrollSync();
        },
        setUserGesture() {},
        setAccessibilityEnabled() {},
    };

    // ── Signal ready ─────────────────────────────────────────────────────────
    setXtermAppActive(true);
    fitTerminal();
    native.load();
    native.syncFocus();

    function disableWebTextInput() {
        if (!term.textarea)
            return;
        term.textarea.readOnly = true;
        term.textarea.tabIndex = -1;
        term.textarea.autocapitalize = 'off';
        term.textarea.autocomplete = 'off';
        term.textarea.autocorrect = 'off';
        term.textarea.spellcheck = false;
    }

    function selectionCell(x, y) {
        const screen = terminalElement.querySelector('.xterm-screen');
        const cell = getCellSize();
        if (!screen || !cell.width || !cell.height || !Number.isFinite(x) || !Number.isFinite(y))
            return null;
        const rect = screen.getBoundingClientRect();
        return {
            col: Math.max(0, Math.min(term.cols - 1, Math.floor((x - rect.left) / cell.width))),
            row: term.buffer.active.viewportY + Math.max(0, Math.min(term.rows - 1, Math.floor((y - rect.top) / cell.height))),
        };
    }

    function installFocusBridge() {
        terminalElement.addEventListener('touchstart', (event) => {
            if (!term.hasSelection())
                event.preventDefault();
        }, {capture: true});
        terminalElement.addEventListener('touchend', (event) => {
            if (term.hasSelection())
                term.clearSelection();
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

    function installLigatures() {
        try {
            term.loadAddon(new LigaturesAddon());
        } catch (error) {
            native.log(`terminal ligatures addon failed: ${error?.stack || error}`);
        }
    }

    function installNativeFocusAdapter() {
        const browserService = term._core?._coreBrowserService;
        if (!browserService)
            return;
        try {
            Object.defineProperty(browserService, 'isFocused', {
                configurable: true,
                get: () => !!xtermAppActive,
            });
        } catch (error) {
            native.log(`terminal focus adapter failed: ${error?.stack || error}`);
        }
    }

    function setXtermAppActive(active) {
        if (xtermAppActive === active) {
            if (active)
                ensureCursorVisible();
            return;
        }
        xtermAppActive = active;

        const core = term._core;
        try {
            if (core?.element)
                core.element.classList.toggle('focus', active);
            if (active) {
                core?._renderService?.handleFocus?.();
                ensureCursorVisible();
            } else {
                core?._renderService?.handleBlur?.();
            }
        } catch (error) {
            native.log(`terminal focus sync failed: ${error?.stack || error}`);
        }
    }

    function ensureCursorVisible() {
        const coreService = term._core?.coreService;
        if (coreService)
            coreService.isCursorInitialized = true;
        term.refresh(term.buffer.active.cursorY, term.buffer.active.cursorY);
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
        const cellHeight = getCellSize().height;
        if (!cellHeight)
            return;
        const buffer = term.buffer.active;
        // Fit rounds rows down; the unused fraction still belongs to UIKit's
        // viewport. Its maximum offset must equal baseY * cellHeight exactly.
        const scrollHeight = buffer.baseY * cellHeight + terminalElement.clientHeight;
        const scrollTop = buffer.viewportY * cellHeight;

        if (scrollHeight !== lastNativeScrollHeight) {
            native.newScrollHeight(scrollHeight);
            lastNativeScrollHeight = scrollHeight;
        }
        const acknowledgesNativeScroll = nativeScrollRequest?.buffer === buffer &&
            nativeScrollRequest.line === buffer.viewportY &&
            nativeScrollRequest.cellHeight === cellHeight;
        nativeScrollRequest = null;
        if (!acknowledgesNativeScroll && scrollTop !== lastNativeScrollTop) {
            native.newScrollTop(scrollTop);
        }
        lastNativeScrollTop = scrollTop;
    }

    function syncApplicationCursor() {
        syncProp('applicationCursor', !!term.modes?.applicationCursorKeysMode);
    }

    function syncProp(name, value) {
        if (oldProps[name] !== value)
            native.propUpdate(name, value);
        oldProps[name] = value;
    }

    function getCellSize() {
        const dims = term._core?._renderService?.dimensions;
        return {
            width: dims?.css?.cell?.width || 0,
            height: dims?.css?.cell?.height || 0,
        };
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

    async function loadConfiguredFont(style) {
        if (!document.fonts?.load)
            return;
        const family = firstFontFamily(style.fontFamily);
        if (!family)
            return;
        try {
            await Promise.all([
                document.fonts.load(`${style.fontSize}px ${quoteCssFontFamily(family)}`),
                document.fonts.load(`700 ${style.fontSize}px ${quoteCssFontFamily(family)}`),
            ]);
            await document.fonts.ready;
        } catch (error) {
            native.log(`terminal font load failed: ${error}`);
        }
    }

    function firstFontFamily(familyList) {
        const trimmed = String(familyList || '').trim();
        if (!trimmed)
            return '';
        const match = trimmed.match(/^(['"])(.*?)\1/);
        if (match)
            return match[2];
        return trimmed.split(',')[0].trim();
    }

    function quoteCssFontFamily(family) {
        const trimmed = String(family || '').trim();
        if (!trimmed || trimmed.startsWith('"') || trimmed.startsWith("'"))
            return trimmed;
        if (/^[a-zA-Z_-][a-zA-Z0-9_-]*$/.test(trimmed))
            return trimmed;
        return `"${trimmed.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
    }

    function cursorStyleForXterm(cursorShape) {
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

    function themeForXterm(style) {
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

})();
