# Hardware keyboard and pointer forwarding

Hardware Shift now uses UIKit's actual layout-aware printable character while
Control/Option/Command shortcuts retain printable X11 keysyms. Key-up reuses the
key-down symbol. A modifier's own key-down updates its state immediately.

UIKit owns targeted indirect-pointer clicks on iPad, including a Magic Keyboard
that exposes GC movement without button delivery. GC button presence is not
inferred from mouse presence. Relative-only GC presses require a visible, unobscured
framebuffer target independently of existing keyboard focus; an owned release is forwarded even after a source
or presentation transition. Button history remains per viewer.

Host policy checks cover modifiers/layouts, source transitions and pointer
button ownership. These checks do not synthesize real Magic Keyboard hardware.
Earlier manual integration use reported working clicks, typing and Shift; a
standalone physical-device qualification is not claimed by this extraction.

Shifted shortcuts retain UIKit's layout-derived printable character, including
Option/Command combinations. Missing Shift text is not reconstructed through a
US punctuation table; Control's encoded ASCII punctuation is decoded directly.
