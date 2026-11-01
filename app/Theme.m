//
//  Theme.m
//  iSH
//
//  Created by Saagar Jha on 2/25/22.
//

#import "Theme.h"
#import "UserPreferences.h"
#import "fs/proc/ish.h"

char *get_documents_directory_impl(void) {
    return strdup(NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject.UTF8String);
}

#define THEME_VERSION 1

@implementation UIColor (iSH)
- (nullable instancetype)ish_initWithHexString:(NSString *)string {
    if (![string hasPrefix:@"#"]) {
        return nil;
    }
    NSScanner *scanner = [NSScanner scannerWithString:string];
    // Skip the leading #
    [scanner setScanLocation:1];
    unsigned int value;
    if (![scanner scanHexInt:&value] || scanner.scanLocation != string.length) {
        return nil;
    }
    unsigned int red;
    unsigned int green;
    unsigned int blue;
    unsigned int alpha;
    if (string.length == 4) { // RGB
        blue = ((value & 0x00f) >> 0) * 0x11;
        green = ((value & 0x0f0) >> 4) * 0x11;
        red = ((value & 0xf00) >> 8) * 0x11;
        alpha = 0xff;
    } else if (string.length == 5) { // RGBA
        blue = ((value & 0x000f) >> 0) * 0x11;
        green = ((value & 0x00f0) >> 4) * 0x11;
        red = ((value & 0x0f00) >> 8) * 0x11;
        alpha = ((value & 0xf000) >> 12) * 0x11;
    } else if (string.length == 7) { // RRGGBB
        blue = (value & 0x0000ff) >> 0;
        green = (value & 0x00ff00) >> 8;
        red = (value & 0xff0000) >> 16;
        alpha = 0xff;
    } else if (string.length == 9) { // RRGGBBAA
        blue = (value & 0x000000ff) >> 0;
        green = (value & 0x0000ff00) >> 8;
        red = (value & 0x00ff0000) >> 16;
        alpha = (value & 0xff000000) >> 24;
    } else {
        return nil;
    }
    return [UIColor colorWithRed:1.0 * red / 0xff green:1.0 * green / 0xff blue:1.0 * blue / 0xff alpha:1.0 * alpha / 0xff];
}
@end

@interface DirectoryWatcher: NSObject<NSFilePresenter>
@property(readonly, copy) NSURL *presentedItemURL;
- (instancetype)initWithURL:(NSURL *)url handler:(void (^)(void))handler;
@end

@implementation DirectoryWatcher {
    void (^_handler)(void);
}
- (instancetype)initWithURL:(NSURL *)url handler:(void (^)(void))handler {
    if (self = [super init]) {
        self->_presentedItemURL = url;
        self->_handler = handler;
    }
    return self;
}

- (NSOperationQueue *)presentedItemOperationQueue {
    return NSOperationQueue.mainQueue;
}

- (void)presentedItemDidChange {
    self->_handler();
}
@end

@interface Palette ()
@property(readonly, nonnull) NSDictionary *serializedRepresentation;

- (nullable instancetype)initWithSerializedRepresentation:(nonnull NSDictionary *)serializedRepresentation;
@end

@implementation Palette

- (instancetype)initWithForegroundColor:(NSString *)foregroundColor backgroundColor:(NSString *)backgroundColor cursorColor:(NSString *)cursorColor colorPaletteOverrides:(NSArray<NSString *> *)colorPaletteOverrides {
    if (self = [super init]) {
        self->_foregroundColor = foregroundColor;
        self->_backgroundColor = backgroundColor;
        self->_cursorColor = cursorColor;
        self->_colorPaletteOverrides = colorPaletteOverrides;
    }
    return self;
}

- (instancetype)initWithSerializedRepresentation:(NSDictionary *)serializedRepresentation {
#define VALID_COLOR(color) (color && [color isKindOfClass:NSString.class] && [[UIColor alloc] ish_initWithHexString:color])
    id foregroundColor = serializedRepresentation[@"foregroundColor"];
    id backgroundColor = serializedRepresentation[@"backgroundColor"];
    id cursorColor = serializedRepresentation[@"cursorColor"];
    id colorPaletteOverrides = serializedRepresentation[@"colorPaletteOverrides"];
    BOOL validColorPalette = YES;
    if (colorPaletteOverrides) {
        validColorPalette = [colorPaletteOverrides isKindOfClass:NSArray.class] && [colorPaletteOverrides count] == 16;
        if (validColorPalette) {
            for (id color in colorPaletteOverrides) {
                validColorPalette = validColorPalette && VALID_COLOR(color);
            }
        }
    }
    if (VALID_COLOR(foregroundColor) && VALID_COLOR(backgroundColor) && (!cursorColor || VALID_COLOR(cursorColor)) && validColorPalette) {
        return [self initWithForegroundColor:foregroundColor backgroundColor:backgroundColor cursorColor:cursorColor colorPaletteOverrides:colorPaletteOverrides];
    } else {
        return nil;
    }
#undef VALID_COLOR
}

- (NSDictionary *)serializedRepresentation {
    NSMutableDictionary *representation = [@{
        @"foregroundColor": self.foregroundColor,
        @"backgroundColor": self.backgroundColor,
    } mutableCopy];
    if (self.cursorColor) {
        representation[@"cursorColor"] = self.cursorColor;
    }
    if (self.colorPaletteOverrides) {
        representation[@"colorPaletteOverrides"] = self.colorPaletteOverrides;
    }
    return  representation;
}

@end

@interface ThemeAppearance ()
@property(readonly, nonnull) NSDictionary *serializedRepresentation;

- (nullable instancetype)initWithSerializedRepresentation:(nonnull NSDictionary *)serializedRepresentation;
@end

@implementation ThemeAppearance

- (instancetype)initWithLightOverride:(BOOL)lightOverride darkOverride:(BOOL)darkOverride {
    if (self = [super init]) {
        self->_lightOverride = lightOverride;
        self->_darkOverride = darkOverride;
    }
    return self;
}

- (instancetype)initWithSerializedRepresentation:(NSDictionary *)serializedRepresentation {
    id lightOverride = serializedRepresentation[@"lightOverride"];
    id darkOverride = serializedRepresentation[@"darkOverride"];
    if ([lightOverride isKindOfClass:NSNumber.class] && [darkOverride isKindOfClass:NSNumber.class]) {
        return [self initWithLightOverride:[lightOverride boolValue] darkOverride:[darkOverride boolValue]];
    } else {
        return nil;
    }
}

+ (instancetype)alwaysLight {
    return [[self alloc] initWithLightOverride:NO darkOverride:YES];
}

+ (instancetype)alwaysDark {
    return [[self alloc] initWithLightOverride:YES darkOverride:NO];
}

- (NSDictionary *)serializedRepresentation {
    return @{
        @"lightOverride": @(self.lightOverride),
        @"darkOverride": @(self.darkOverride),
    };
}

@end

DirectoryWatcher *directoryWatcher;
NSString *const ThemesUpdatedNotification = @"ThemesUpdatedNotification";
NSString *const ThemeUpdatedNotification = @"ThemeUpdatedNotification";

@interface Theme ()
@property(readonly, nonnull) NSData *data;
@end

// TODO: Move these to Linux
#if ISH_LINUX
char *(*get_documents_directory)(void);
#endif

@implementation Theme
+ (void)initialize {
    directoryWatcher = [[DirectoryWatcher alloc] initWithURL:self.themesDirectory handler:^{
        [NSNotificationCenter.defaultCenter postNotificationName:ThemesUpdatedNotification object:nil];
    }];
    [NSFileCoordinator addFilePresenter:directoryWatcher];
    
    get_documents_directory = get_documents_directory_impl;
    [NSFileManager.defaultManager createDirectoryAtURL:self.themesDirectory withIntermediateDirectories:YES attributes:nil error:nil];
}

- (instancetype)initWithName:(NSString *)name palette:(Palette *)palette appearance:(ThemeAppearance *)appearance {
    Theme *theme = [self initWithName:name lightPalette:palette darkPalette:palette appearance:appearance];
    return theme;
}

- (instancetype)initWithName:(NSString *)name lightPalette:(nonnull Palette *)lightPalette darkPalette:(nonnull Palette *)darkPalette appearance:(nullable ThemeAppearance *)appearance {
    if (self = [super init]) {
        self->_name = name;
        self->_lightPalette = lightPalette;
        self->_darkPalette = darkPalette;
        self->_appearance = appearance;
    }
    return self;
}

- (nullable instancetype)initWithName:(NSString *)name data:(NSData *)data {
    id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![json isKindOfClass:NSDictionary.class]) {
        return nil;
    }
    id version = json[@"version"];
    if (![version isKindOfClass:NSNumber.class] || ((NSNumber *)version).integerValue <= 0 || ((NSNumber *)version).integerValue > THEME_VERSION) {
        NSLog(@"Rejecting theme %@ with invalid version number", name);
        return nil;
    }
    id _appearance = json[@"appearance"];
    ThemeAppearance *appearance = [_appearance isKindOfClass:NSDictionary.class] ? [[ThemeAppearance alloc] initWithSerializedRepresentation:_appearance] : nil;
    id shared = json[@"shared"];
    id light = json[@"light"];
    id dark = json[@"dark"];
    if ([shared isKindOfClass:NSDictionary.class]) {
        Palette *palette = [[Palette alloc] initWithSerializedRepresentation:shared];
        return palette ? [self initWithName:name palette:palette appearance:appearance] : nil;
    } else if ([light isKindOfClass:NSDictionary.class] && [dark isKindOfClass:NSDictionary.class]) {
        Palette *lightPalette = [[Palette alloc] initWithSerializedRepresentation:light];
        Palette *darkPalette = [[Palette alloc] initWithSerializedRepresentation:dark];
        return lightPalette && darkPalette ? [self initWithName:name lightPalette:lightPalette darkPalette:darkPalette appearance:appearance] : nil;
    } else {
        NSLog(@"Rejecting theme %@ with invalid palette(s)", name);
        return nil;
    }
}

+ (Theme *)themeWithName:(NSString *)name foreground:(NSString *)foreground background:(NSString *)background cursor:(NSString *)cursor appearance:(ThemeAppearance *)appearance palette:(NSArray<NSString *> *)palette {
    return [[self alloc] initWithName:name
                              palette:[[Palette alloc] initWithForegroundColor:foreground
                                                               backgroundColor:background
                                                                   cursorColor:cursor
                                                         colorPaletteOverrides:palette]
                           appearance:appearance];
}

+ (NSArray<Theme *> *)defaultThemes {
    static NSArray<Theme *> *defaultThemes;
    if (!defaultThemes) {
        defaultThemes = @[
            // LinuxRunner: Nord palette on a deeper polar-night background,
            // frost cursor; matches the Hyprland and Wayfire presets.
            [self themeWithName:@"linuxrunner"
                  foreground:@"#d8dee9"
                  background:@"#242933"
                      cursor:@"#88c0d0"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#3b4252",
                            @"#bf616a",
                            @"#a3be8c",
                            @"#ebcb8b",
                            @"#81a1c1",
                            @"#b48ead",
                            @"#88c0d0",
                            @"#e5e9f0",
                            @"#4c566a",
                            @"#d08770",
                            @"#b1d196",
                            @"#f0d399",
                            @"#8fbcbb",
                            @"#c895bf",
                            @"#93ccdc",
                            @"#eceff4",
                      ]],
            [self themeWithName:@"xterm"
                  foreground:@"#e5e5e5"
                  background:@"#000000"
                      cursor:@"#e5e5e5"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#000000",
                            @"#cd0000",
                            @"#00cd00",
                            @"#cdcd00",
                            @"#0000cd",
                            @"#cd00cd",
                            @"#00cdcd",
                            @"#e5e5e5",
                            @"#4d4d4d",
                            @"#ff0000",
                            @"#00ff00",
                            @"#ffff00",
                            @"#0000ff",
                            @"#ff00ff",
                            @"#00ffff",
                            @"#ffffff",
                      ]],
            [self themeWithName:@"tango"
                  foreground:@"#ffffff"
                  background:@"#000000"
                      cursor:@"#ffffff"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#000000",
                            @"#cc0000",
                            @"#4e9a06",
                            @"#c4a000",
                            @"#3465a4",
                            @"#75507b",
                            @"#06989a",
                            @"#d3d7cf",
                            @"#555753",
                            @"#ef2929",
                            @"#8ae234",
                            @"#fce94f",
                            @"#729fcf",
                            @"#ad7fa8",
                            @"#34e2e2",
                            @"#eeeeec",
                      ]],
            [self themeWithName:@"monokai"
                  foreground:@"#fdfff1"
                  background:@"#272822"
                      cursor:@"#fdfff1"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#272822",
                            @"#f92672",
                            @"#a6e22e",
                            @"#e6db74",
                            @"#fd971f",
                            @"#ae81ff",
                            @"#66d9ef",
                            @"#fdfff1",
                            @"#6e7066",
                            @"#f92672",
                            @"#a6e22e",
                            @"#e6db74",
                            @"#fd971f",
                            @"#ae81ff",
                            @"#66d9ef",
                            @"#fdfff1",
                      ]],
            [self themeWithName:@"monokai-pro"
                  foreground:@"#fcfcfa"
                  background:@"#2d2a2e"
                      cursor:@"#fcfcfa"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#403e41",
                            @"#ff6188",
                            @"#a9dc76",
                            @"#ffd866",
                            @"#fc9867",
                            @"#ab9df2",
                            @"#78dce8",
                            @"#fcfcfa",
                            @"#727072",
                            @"#ff6188",
                            @"#a9dc76",
                            @"#ffd866",
                            @"#fc9867",
                            @"#ab9df2",
                            @"#78dce8",
                            @"#fcfcfa",
                      ]],
            [self themeWithName:@"ristretto"
                  foreground:@"#fff1f3"
                  background:@"#2c2525"
                      cursor:@"#fff1f3"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#2c2525",
                            @"#fd6883",
                            @"#adda78",
                            @"#f9cc6c",
                            @"#f38d70",
                            @"#a8a9eb",
                            @"#85dacc",
                            @"#fff1f3",
                            @"#72696a",
                            @"#fd6883",
                            @"#adda78",
                            @"#f9cc6c",
                            @"#f38d70",
                            @"#a8a9eb",
                            @"#85dacc",
                            @"#fff1f3",
                      ]],
            [self themeWithName:@"dark"
                  foreground:@"#d4d4d4"
                  background:@"#1e1e1e"
                      cursor:@"#d4d4d4"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#000000",
                            @"#cd3131",
                            @"#0dbc79",
                            @"#e5e510",
                            @"#2472c8",
                            @"#bc3fbc",
                            @"#11a8cd",
                            @"#e5e5e5",
                            @"#666666",
                            @"#f14c4c",
                            @"#23d18b",
                            @"#f5f543",
                            @"#3b8eea",
                            @"#d670d6",
                            @"#29b8db",
                            @"#ffffff",
                      ]],
            [self themeWithName:@"light"
                  foreground:@"#383a42"
                  background:@"#ffffff"
                      cursor:@"#383a42"
                  appearance:ThemeAppearance.alwaysLight
                     palette:@[
                            @"#000000",
                            @"#e45649",
                            @"#50a14f",
                            @"#c18401",
                            @"#4078f2",
                            @"#a626a4",
                            @"#0184bc",
                            @"#a0a1a7",
                            @"#5c6370",
                            @"#e06c75",
                            @"#98c379",
                            @"#d19a66",
                            @"#61afef",
                            @"#c678dd",
                            @"#56b6c2",
                            @"#ffffff",
                      ]],
            [self themeWithName:@"dracula"
                  foreground:@"#f8f8f2"
                  background:@"#282a36"
                      cursor:@"#f8f8f2"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#21222c",
                            @"#ff5555",
                            @"#50fa7b",
                            @"#f1fa8c",
                            @"#bd93f9",
                            @"#ff79c6",
                            @"#8be9fd",
                            @"#f8f8f2",
                            @"#6272a4",
                            @"#ff6e6e",
                            @"#69ff94",
                            @"#ffffa5",
                            @"#d6acff",
                            @"#ff92df",
                            @"#a4ffff",
                            @"#ffffff",
                      ]],
            [self themeWithName:@"catppuccin"
                  foreground:@"#cdd6f4"
                  background:@"#1e1e2e"
                      cursor:@"#cdd6f4"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#45475a",
                            @"#f38ba8",
                            @"#a6e3a1",
                            @"#f9e2af",
                            @"#89b4fa",
                            @"#f5c2e7",
                            @"#94e2d5",
                            @"#bac2de",
                            @"#585b70",
                            @"#f38ba8",
                            @"#a6e3a1",
                            @"#f9e2af",
                            @"#89b4fa",
                            @"#f5c2e7",
                            @"#94e2d5",
                            @"#a6adc8",
                      ]],
            [self themeWithName:@"nord"
                  foreground:@"#d8dee9"
                  background:@"#2e3440"
                      cursor:@"#d8dee9"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#3b4252",
                            @"#bf616a",
                            @"#a3be8c",
                            @"#ebcb8b",
                            @"#81a1c1",
                            @"#b48ead",
                            @"#88c0d0",
                            @"#e5e9f0",
                            @"#4c566a",
                            @"#bf616a",
                            @"#a3be8c",
                            @"#ebcb8b",
                            @"#81a1c1",
                            @"#b48ead",
                            @"#8fbcbb",
                            @"#eceff4",
                      ]],
            [self themeWithName:@"gruvbox"
                  foreground:@"#ebdbb2"
                  background:@"#282828"
                      cursor:@"#ebdbb2"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#282828",
                            @"#cc241d",
                            @"#98971a",
                            @"#d79921",
                            @"#458588",
                            @"#b16286",
                            @"#689d6a",
                            @"#a89984",
                            @"#928374",
                            @"#fb4934",
                            @"#b8bb26",
                            @"#fabd2f",
                            @"#83a598",
                            @"#d3869b",
                            @"#8ec07c",
                            @"#ebdbb2",
                      ]],
            [self themeWithName:@"solarized"
                  foreground:@"#839496"
                  background:@"#002b36"
                      cursor:@"#839496"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#073642",
                            @"#dc322f",
                            @"#859900",
                            @"#b58900",
                            @"#268bd2",
                            @"#d33682",
                            @"#2aa198",
                            @"#eee8d5",
                            @"#586e75",
                            @"#cb4b16",
                            @"#586e75",
                            @"#657b83",
                            @"#839496",
                            @"#6c71c4",
                            @"#93a1a1",
                            @"#fdf6e3",
                      ]],
            [self themeWithName:@"miasma"
                  foreground:@"#c2c2b0"
                  background:@"#222222"
                      cursor:@"#c2c2b0"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#000000",
                            @"#685742",
                            @"#5f875f",
                            @"#b36d43",
                            @"#78824b",
                            @"#bb7744",
                            @"#c9a554",
                            @"#d7c483",
                            @"#666666",
                            @"#685742",
                            @"#5f875f",
                            @"#b36d43",
                            @"#78824b",
                            @"#bb7744",
                            @"#c9a554",
                            @"#d7c483",
                      ]],
            [self themeWithName:@"github"
                  foreground:@"#adbac7"
                  background:@"#1c2128"
                      cursor:@"#adbac7"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#545d68",
                            @"#f47067",
                            @"#57ab5a",
                            @"#c69026",
                            @"#539bf5",
                            @"#b083f0",
                            @"#39c5cf",
                            @"#909dab",
                            @"#636e7b",
                            @"#ff938a",
                            @"#6bc46d",
                            @"#daaa3f",
                            @"#6cb6ff",
                            @"#dcbdfb",
                            @"#56d4dd",
                            @"#cdd9e5",
                      ]],
            [self themeWithName:@"gotham"
                  foreground:@"#99d1ce"
                  background:@"#0c1014"
                      cursor:@"#99d1ce"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#0c1014",
                            @"#c23127",
                            @"#2aa889",
                            @"#edb443",
                            @"#195466",
                            @"#4e5166",
                            @"#33859e",
                            @"#99d1ce",
                            @"#0c1014",
                            @"#c23127",
                            @"#2aa889",
                            @"#edb443",
                            @"#195466",
                            @"#4e5166",
                            @"#33859e",
                            @"#99d1ce",
                      ]],
            [self themeWithName:@"tokyo"
                  foreground:@"#a9b1d6"
                  background:@"#1a1b26"
                      cursor:@"#a9b1d6"
                  appearance:ThemeAppearance.alwaysDark
                     palette:@[
                            @"#15161e",
                            @"#f7768e",
                            @"#9ece6a",
                            @"#e0af68",
                            @"#7aa2f7",
                            @"#bb9af7",
                            @"#7dcfff",
                            @"#a9b1d6",
                            @"#414868",
                            @"#f7768e",
                            @"#9ece6a",
                            @"#e0af68",
                            @"#7aa2f7",
                            @"#bb9af7",
                            @"#7dcfff",
                            @"#c0caf5",
                      ]],
        ];
    }
    return defaultThemes;
}

+ (NSURL *)themesDirectory {
    return [[NSURL fileURLWithPath:NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject] URLByAppendingPathComponent:@"themes"];
}

+ (NSArray<Theme *> *)userThemes {
    NSMutableArray<Theme *> *themes = [NSMutableArray new];
    for (NSURL *file in [NSFileManager.defaultManager contentsOfDirectoryAtURL:self.themesDirectory includingPropertiesForKeys:nil options:0 error:nil]) {
        NSData *data = [NSData dataWithContentsOfURL:file];
        if (!data) {
            continue;
        }
        
        Theme *theme = [[Theme alloc] initWithName:file.lastPathComponent.stringByDeletingPathExtension data:data];
        if (theme) {
            [themes addObject:theme];
        }
    }
    [themes sortUsingDescriptors:@[[NSSortDescriptor sortDescriptorWithKey:@"name" ascending:YES selector:@selector(localizedStandardCompare:)]]];
    return themes;
}

- (NSData *)data {
    NSMutableDictionary *representation = [@{
        @"version": @(THEME_VERSION),
    } mutableCopy];
    if (self.lightPalette == self.darkPalette) {
        representation[@"shared"] = self.lightPalette.serializedRepresentation;
    } else {
        representation[@"light"] = self.lightPalette.serializedRepresentation;
        representation[@"dark"] = self.darkPalette.serializedRepresentation;
    }
    if (self.appearance) {
        representation[@"appearance"] = self.appearance.serializedRepresentation;
    }
    return [NSJSONSerialization dataWithJSONObject:representation options:NSJSONWritingSortedKeys | NSJSONWritingPrettyPrinted error:nil];
}

+ (Theme *)themeForName:(NSString *)name includingDefaultThemes:(BOOL)includingDefaultThemes {
    // We should pick user themes over default ones, if they have the same name.
    NSMutableArray<Theme *> *themes = [Theme.userThemes mutableCopy];
    if (includingDefaultThemes) {
        [themes addObjectsFromArray:Theme.defaultThemes];
    }
    for (Theme *theme in themes) {
        if ([theme.name isEqualToString:name]) {
            return theme;
        }
    }
    return nil;
}

- (void)duplicateAsUserTheme {
    NSString *name;
    for (int suffix = 1; [self.class themeForName:name = [NSString stringWithFormat:@"%@-%d", self.name, suffix] includingDefaultThemes:NO]; ++suffix);
    [self.data writeToURL:[self.class.themesDirectory URLByAppendingPathComponent:[name stringByAppendingString:@".json"]] atomically:YES];
}

- (BOOL)addUserTheme {
    if ([self.class themeForName:self.name includingDefaultThemes:NO]) {
        return NO;
    } else {
        [self.data writeToURL:[self.class.themesDirectory URLByAppendingPathComponent:[self.name stringByAppendingString:@".json"]] atomically:YES];
        return YES;
    }
}

- (void)deleteUserTheme {
    [NSFileManager.defaultManager removeItemAtURL:[self.class.themesDirectory URLByAppendingPathComponent:[self.name stringByAppendingString:@".json"]] error:nil];
}

- (void)replaceWithUserTheme:(Theme *)theme {
    [theme.data writeToURL:[self.class.themesDirectory URLByAppendingPathComponent:[theme.name stringByAppendingString:@".json"]] atomically:YES];
    if (![self.name isEqualToString:theme.name]) {
        [self deleteUserTheme];
        [NSNotificationCenter.defaultCenter postNotificationName:ThemeUpdatedNotification object:theme.name];
    }
}
@end
