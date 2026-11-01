//
//  UserPreferences.m
//  iSH
//
//  Created by Charlie Melbye on 11/12/18.
//

#import "UserPreferences.h"
#import "fs/proc/ish.h"

// IMPORTANT: If you add a constant here and expose it via UserPreferences,
// consider if it also needs to be exposed as a friendly preference and included
// in the KVO list below. (In most circumstances, the answer is "yes".)
static NSString *const kPreferenceCapsLockMappingKey = @"Caps Lock Mapping";
static NSString *const kPreferenceOptionMappingKey = @"Option Mapping";
static NSString *const kPreferenceBacktickEscapeKey = @"Backtick Mapping Escape";
static NSString *const kPreferenceHideExtraKeysWithExternalKeyboardKey = @"Hide Extra Keys With External Keyboard";
static NSString *const kPreferenceOverrideControlSpaceKey = @"Override Control Space";
static NSString *const kPreferenceFontFamilyKey = @"Font Family";
static NSString *const kPreferenceFontSizeKey = @"Font Size";
static NSString *const kPreferenceThemeKey = @"ModernTheme";
static NSString *const kPreferenceDisableDimmingKey = @"Disable Dimming";
NSString *const kPreferenceLaunchCommandKey = @"Init Command";
NSString *const kPreferenceBootCommandKey = @"Boot Command";
static NSString *const kPreferenceCursorStyleKey = @"Cursor Style";
static NSString *const kPreferenceBlinkCursorKey = @"Blink Cursor";
NSString *const kPreferenceHideStatusBarKey = @"Status Bar";
static NSString *const kPreferenceColorSchemeKey = @"Color Scheme";
// This key has a different naming scheme because it's used in UI tests as a
// CLI argument.
NSString *const kHostnameOverrideKey = @"hostnameOverride";

NSDictionary<NSString *, NSString *> *friendlyPreferenceMapping;
NSDictionary<NSString *, NSString *> *friendlyPreferenceReverseMapping;
NSDictionary<NSString *, NSString *> *kvoProperties;

static NSString *const kSystemMonospacedFontName = @"ui-monospace";
static NSString *const kFiraTerminalFontFamilyName = @"FiraCode Nerd Font Mono";
static NSString *const kJetBrainsTerminalFontFamilyName = @"JetBrainsMono Nerd Font Mono";
static NSString *const kLegacyGhosttyWebTerminalFontFamilyName = @"\"FiraCode Nerd Font Mono\", Menlo, Monaco, \"Courier New\", monospace";
static NSString *const kLegacyWebtermTerminalFontFamilyName = @"ui-monospace, \"SFMono-Regular\", \"FiraCode Nerd Font\", \"FiraMono Nerd Font\", \"FiraCode Nerd Font Mono\", \"Fira Code\", \"Roboto Mono\", Menlo, Monaco, Consolas, \"Liberation Mono\", \"DejaVu Sans Mono\", \"Courier New\", monospace";
static NSString *const kWebtermTerminalFontFamilyName = @"\"JetBrainsMono Nerd Font Mono\", \"FiraCode Nerd Font Mono\", ui-monospace, \"SFMono-Regular\", Menlo, Monaco, monospace";

@interface UserPreferences () {
    BOOL _hostnameIsOverridden;
}
- (void)updateTheme;
@end

char **get_all_defaults_keys_impl(void) {
    NSArray<NSString *> *preferenceKeys = NSUserDefaults.standardUserDefaults.dictionaryRepresentation.allKeys;
    char **entries = malloc((preferenceKeys.count + 1) * sizeof(*entries));
    for (NSUInteger i = 0; i < preferenceKeys.count; ++i)
        entries[i] = strdup(preferenceKeys[i].UTF8String);
    entries[preferenceKeys.count] = NULL;
    return entries;
}

char *get_friendly_name_impl(const char *name) {
    const char *friendly_name = friendlyPreferenceReverseMapping[[NSString stringWithUTF8String:name]].UTF8String;
    if (friendly_name == NULL)
        return NULL;
    return strdup(friendly_name);
}

char *get_underlying_name_impl(const char *name) {
    return strdup(friendlyPreferenceMapping[[NSString stringWithUTF8String:name]].UTF8String);
}

bool get_user_default_impl(const char *name, char **buffer, size_t *size) {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:[NSString stringWithUTF8String:name]];
    // Since we are writing with fragments, wrap the object in an array to have
    // a top-level object to check.
    if (!value || ![NSJSONSerialization isValidJSONObject:@[value]]) {
        return false;
    }
    NSError *error;
    NSJSONWritingOptions options = NSJSONWritingFragmentsAllowed | NSJSONWritingSortedKeys | NSJSONWritingPrettyPrinted;
    if (@available(iOS 13.0, *)) {
        options |= NSJSONWritingWithoutEscapingSlashes;
    }
    NSData *data = [NSJSONSerialization dataWithJSONObject:value options:options error:&error];
    if (error) {
        return false;
    }
    *buffer = malloc(data.length + 1);
    memcpy(*buffer, data.bytes, data.length);
    (*buffer)[data.length] = '\n';
    *size = data.length + 1;
    return true;
}

bool set_user_default_impl(const char *name, char *buffer, size_t size) {
    NSString *key = [NSString stringWithUTF8String:name];
    NSData *data = [NSData dataWithBytesNoCopy:buffer length:size freeWhenDone:NO];
    NSError *error;
    id value = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingFragmentsAllowed error:&error];
    if (error) {
        return false;
    }
    NSString *property = kvoProperties[key];
    if (property) {
        if ([UserPreferences.shared validateValue:&value forKey:property error:nil]) {
            [UserPreferences.shared setValue:value forKey:property];
        } else {
            return false;
        }
    } else {
        [NSUserDefaults.standardUserDefaults setValue:value forKey:key];
    }
    return true;
}

bool remove_user_default_impl(const char *name) {
    NSString *key = [NSString stringWithUTF8String:name];
    NSString *property = kvoProperties[key];
    if (property) {
        [UserPreferences.shared willChangeValueForKey:property];
    }
    [NSUserDefaults.standardUserDefaults removeObjectForKey:key];
    if (property) {
        [UserPreferences.shared didChangeValueForKey:property];
    }
    
    // This particular property needs special handling to stay up-to-date
    if ([property isEqualToString:@"userTheme"]) {
        [UserPreferences.shared updateTheme];
    }
    return true;
}

// TODO: Move these to Linux
#if ISH_LINUX
char **(*get_all_defaults_keys)(void);
char *(*get_friendly_name)(const char *name);
char *(*get_underlying_name)(const char *name);
bool (*get_user_default)(const char *name, char **buffer, size_t *size);
bool (*set_user_default)(const char *name, char *buffer, size_t size);
bool (*remove_user_default)(const char *name);
#endif

@implementation UserPreferences {
    NSUserDefaults *_defaults;
}

+ (instancetype)shared {
    static UserPreferences *shared = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        shared = [[self alloc] init];
    });
    return shared;
}

- (instancetype)init {
    self = [super init];
    self->_hostnameIsOverridden = !![NSUserDefaults.standardUserDefaults stringForKey:kHostnameOverrideKey];
    if (self) {
        _defaults = [NSUserDefaults standardUserDefaults];
        [_defaults registerDefaults:@{
            kPreferenceFontSizeKey: @(12),
            kPreferenceCapsLockMappingKey: @(CapsLockMapControl),
            kPreferenceOptionMappingKey: @(OptionMapNone),
            kPreferenceBacktickEscapeKey: @(NO),
            kPreferenceHideExtraKeysWithExternalKeyboardKey: @(NO),
            kPreferenceOverrideControlSpaceKey: @(NO),
            kPreferenceDisableDimmingKey: @(NO),
            kPreferenceLaunchCommandKey: @[@"/bin/login", @"-f", @"root"],
            kPreferenceBootCommandKey: @[@"/sbin/init"],
            kPreferenceBlinkCursorKey: @(NO),
            kPreferenceCursorStyleKey: @(CursorStyleBlock),
            kPreferenceHideStatusBarKey: @(NO),
            kPreferenceColorSchemeKey: @(ColorSchemeMatchSystem),
            kPreferenceThemeKey: @"linuxrunner",
            kHostnameOverrideKey: UIDevice.currentDevice.name,
        }];
        [_defaults registerDefaults:@{
            kPreferenceFontFamilyKey: kWebtermTerminalFontFamilyName,
        }];
        get_all_defaults_keys = get_all_defaults_keys_impl;
        get_friendly_name = get_friendly_name_impl;
        get_underlying_name = get_underlying_name_impl;
        get_user_default = get_user_default_impl;
        set_user_default = set_user_default_impl;
        remove_user_default = remove_user_default_impl;
        friendlyPreferenceMapping = @{
            @"caps_lock_mapping": kPreferenceCapsLockMappingKey,
            @"option_mapping": kPreferenceOptionMappingKey,
            @"backtick_mapping_escape": kPreferenceBacktickEscapeKey,
            @"hide_extra_keys_with_external_keyboard": kPreferenceHideExtraKeysWithExternalKeyboardKey,
            @"override_control_space": kPreferenceOverrideControlSpaceKey,
            @"font_family": kPreferenceFontFamilyKey,
            @"font_size": kPreferenceFontSizeKey,
            @"disable_dimming": kPreferenceDisableDimmingKey,
            @"launch_command": kPreferenceLaunchCommandKey,
            @"boot_command": kPreferenceBootCommandKey,
            @"cursor_style": kPreferenceCursorStyleKey,
            @"blink_cursor": kPreferenceBlinkCursorKey,
            @"hide_status_bar": kPreferenceHideStatusBarKey,
            @"color_scheme": kPreferenceColorSchemeKey,
            @"theme": kPreferenceThemeKey,
            @"hostname_override": kHostnameOverrideKey,
        };
        NSMutableDictionary <NSString *, NSString *> *reverseMapping = [NSMutableDictionary new];
        for (NSString *key in friendlyPreferenceMapping) {
            reverseMapping[friendlyPreferenceMapping[key]] = key;
        }
        friendlyPreferenceReverseMapping = reverseMapping;
        // Helps a bit with compile-time safety and autocompletion
#define property(x) NSStringFromSelector(@selector(x))
        kvoProperties = @{
            kPreferenceCapsLockMappingKey: property(capsLockMapping),
            kPreferenceOptionMappingKey: property(optionMapping),
            kPreferenceBacktickEscapeKey: property(backtickMapEscape),
            kPreferenceHideExtraKeysWithExternalKeyboardKey: property(hideExtraKeysWithExternalKeyboard),
            kPreferenceOverrideControlSpaceKey: property(overrideControlSpace),
            kPreferenceFontFamilyKey: property(fontFamily),
            kPreferenceFontSizeKey: property(fontSize),
            kPreferenceDisableDimmingKey: property(shouldDisableDimming),
            kPreferenceLaunchCommandKey: property(launchCommand),
            kPreferenceBootCommandKey: property(bootCommand),
            kPreferenceCursorStyleKey: property(cursorStyle),
            kPreferenceBlinkCursorKey: property(blinkCursor),
            kPreferenceHideStatusBarKey: property(hideStatusBar),
            kPreferenceColorSchemeKey: property(colorScheme),
            // This one is a little bit special, so it needs extra handling.
            // The backing property for this is intentionally underscored.
            kPreferenceThemeKey: @"userTheme",
        };
#undef property
        
        [self updateTheme];
        
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateTheme:) name:ThemesUpdatedNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(updateTheme:) name:ThemeUpdatedNotification object:nil];
    }
    return self;
}

// MARK: - Preference properties

// MARK: capsLockMapping
- (CapsLockMapping)capsLockMapping {
    return [_defaults integerForKey:kPreferenceCapsLockMappingKey];
}

- (void)setCapsLockMapping:(CapsLockMapping)capsLockMapping {
    [_defaults setInteger:capsLockMapping forKey:kPreferenceCapsLockMappingKey];
}

- (BOOL)validateCapsLockMapping:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __CapsLockMapFirst && _value < __CapsLockMapLast;
}

// MARK: optionMapping
- (OptionMapping)optionMapping {
    return [_defaults integerForKey:kPreferenceOptionMappingKey];
}

- (void)setOptionMapping:(OptionMapping)optionMapping {
    [_defaults setInteger:optionMapping forKey:kPreferenceOptionMappingKey];
}

- (BOOL)validateOptionMapping:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __OptionMapFirst && _value < __OptionMapLast;
}

// MARK: backtickMapEscape
- (BOOL)backtickMapEscape {
    return [_defaults boolForKey:kPreferenceBacktickEscapeKey];
}

- (void)setBacktickMapEscape:(BOOL)backtickMapEscape {
    [_defaults setBool:backtickMapEscape forKey:kPreferenceBacktickEscapeKey];
}

- (BOOL)validateBacktickMapEscape:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: hideExtraKeysWithExternalKeyboard
- (BOOL)hideExtraKeysWithExternalKeyboard {
    return [_defaults boolForKey:kPreferenceHideExtraKeysWithExternalKeyboardKey];
}

- (void)setHideExtraKeysWithExternalKeyboard:(BOOL)hideExtraKeysWithExternalKeyboard {
    [_defaults setBool:hideExtraKeysWithExternalKeyboard forKey:kPreferenceHideExtraKeysWithExternalKeyboardKey];
}

- (BOOL)validateHideExtraKeysWithExternalKeyboard:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: overrideControlSpace
- (BOOL)overrideControlSpace {
    return [_defaults boolForKey:kPreferenceOverrideControlSpaceKey];
}

- (void)setOverrideControlSpace:(BOOL)overrideControlSpace {
    [_defaults setBool:overrideControlSpace forKey:kPreferenceOverrideControlSpaceKey];
}

- (BOOL)validateOverrideControlSpace:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: fontSize
- (NSNumber *)fontSize {
    return [_defaults objectForKey:kPreferenceFontSizeKey];
}

- (void)setFontSize:(NSNumber *)fontSize {
    [_defaults setObject:fontSize forKey:kPreferenceFontSizeKey];
}

- (BOOL)validateFontSize:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: fontFamily
- (NSString *)fontFamily {
    NSString *fontFamily = [_defaults objectForKey:kPreferenceFontFamilyKey];
    if ([fontFamily isEqualToString:kLegacyGhosttyWebTerminalFontFamilyName] ||
        [fontFamily isEqualToString:kLegacyWebtermTerminalFontFamilyName]) {
        fontFamily = kWebtermTerminalFontFamilyName;
        [_defaults setObject:fontFamily forKey:kPreferenceFontFamilyKey];
    } else if ([fontFamily isEqualToString:@"FiraCode Nerd Font"] ||
               [fontFamily isEqualToString:@"FiraMono Nerd Font"]) {
        fontFamily = kFiraTerminalFontFamilyName;
        [_defaults setObject:fontFamily forKey:kPreferenceFontFamilyKey];
    }
    return fontFamily;
}

- (void)setFontFamily:(NSString *)fontFamily {
    if (fontFamily) {
        [_defaults setObject:fontFamily forKey:kPreferenceFontFamilyKey];
    } else {
        [_defaults removeObjectForKey:kPreferenceFontFamilyKey];
    }
}

- (BOOL)validateFontFamily:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

- (NSString *)fontFamilyUserFacingName {
    if ([self.fontFamily isEqualToString:kWebtermTerminalFontFamilyName]) {
        return @"Webterm Default";
    }
    if ([self.fontFamily isEqualToString:kJetBrainsTerminalFontFamilyName]) {
        return @"JetBrains Mono Nerd Font";
    }
    if ([self.fontFamily isEqualToString:kFiraTerminalFontFamilyName]) {
        return @"FiraCode Nerd Font Mono";
    }
    if ([self.fontFamily isEqualToString:kSystemMonospacedFontName]) {
        return @"System Monospace";
    }
    return self.fontFamily;
}

- (UIFont *)approximateFont {
    if ([self.fontFamily isEqualToString:kWebtermTerminalFontFamilyName] ||
        [self.fontFamily isEqualToString:kJetBrainsTerminalFontFamilyName]) {
        UIFont *font = [UIFont fontWithName:@"JetBrainsMonoNFM-Regular" size:self.fontSize.doubleValue];
        if (font) {
            return font;
        }
    }
    if ([self.fontFamily isEqualToString:kFiraTerminalFontFamilyName]) {
        UIFont *font = [UIFont fontWithName:kFiraTerminalFontFamilyName size:self.fontSize.doubleValue];
        if (font) {
            return font;
        }
    }
    if (@available(iOS 13.4, *)) {
        if ([self.fontFamily isEqualToString:kSystemMonospacedFontName]) {
            return [UIFont monospacedSystemFontOfSize:self.fontSize.doubleValue weight:UIFontWeightRegular];
        }
    }
    UIFont *font = [UIFont fontWithName:self.fontFamily size:self.fontSize.doubleValue];
    if (font) {
        return font;
    }
    UIFontDescriptor *descriptor = [UIFontDescriptor fontDescriptorWithFontAttributes:@{UIFontDescriptorFamilyAttribute: self.fontFamily}];
    font = [UIFont fontWithDescriptor:descriptor size:self.fontSize.doubleValue];
    return font ? font : [UIFont fontWithName:@"Menlo" size:self.fontSize.doubleValue];
}

// MARK: theme
- (void)setTheme:(Theme *)theme {
    _theme = theme;
    [_defaults setObject:theme.name forKey:kPreferenceThemeKey];
}

// These are provided because user theme validation is done with strings
- (NSString *)_userTheme {
    return self.theme.name;
}

- (void)_setUserTheme:(NSString *)userTheme {
    Theme *theme;
    if ((theme = [Theme themeForName:userTheme includingDefaultThemes:YES])) {
        self.theme = theme;
    } else {
        self.theme = Theme.defaultThemes.firstObject;
    }
}

- (BOOL)validateUserTheme:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

- (void)updateTheme:(NSNotification *)notification {
    if (notification.object) {
        [_defaults setValue:notification.object forKey:kPreferenceThemeKey];
    }
    [self updateTheme];
}

- (void)updateTheme {
    [self _setUserTheme:[_defaults valueForKey:kPreferenceThemeKey]];
}

- (Palette *)palette {
    switch (self.colorScheme) {
        case ColorSchemeMatchSystem:
            return self.class.systemThemeIsDark ? self.theme.darkPalette : self.theme.lightPalette;
        case ColorSchemeAlwaysDark:
            return self.theme.darkPalette;
        default:
            NSAssert(NO, @"invalid color scheme");
        case ColorSchemeAlwaysLight:
            return self.theme.lightPalette;
    }
}

// MARK: shouldDisableDimming
- (BOOL)shouldDisableDimming {
    return [_defaults boolForKey:kPreferenceDisableDimmingKey];
}

- (void)setShouldDisableDimming:(BOOL)dim {
    [_defaults setBool:dim forKey:kPreferenceDisableDimmingKey];
}

- (BOOL)validateShouldDisableDimming:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: launchCommand
- (NSArray<NSString *> *)launchCommand {
    return [_defaults stringArrayForKey:kPreferenceLaunchCommandKey];
}

- (void)setLaunchCommand:(NSArray<NSString *> *)launchCommand {
    [_defaults setObject:launchCommand forKey:kPreferenceLaunchCommandKey];
}

- (BOOL)validateLaunchCommand:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSArray.class]) {
        return NO;
    }
    for (id element in (NSArray *)(*value)) {
        if (![element isKindOfClass:NSString.class]) {
            return NO;
        }
    }
    return YES;
}

- (BOOL)hasChangedLaunchCommand {
    NSArray *defaultLaunchCommand = [[[NSUserDefaults alloc] initWithSuiteName:NSRegistrationDomain] stringArrayForKey:kPreferenceLaunchCommandKey];
    return ![self.launchCommand isEqual:defaultLaunchCommand];
}

// MARK: bootCommand
- (NSArray<NSString *> *)bootCommand {
    return [_defaults stringArrayForKey:kPreferenceBootCommandKey];
}

- (void)setBootCommand:(NSArray<NSString *> *)bootCommand {
    [_defaults setObject:bootCommand forKey:kPreferenceBootCommandKey];
}

- (BOOL)validateBootCommand:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSArray.class]) {
        return NO;
    }
    for (id element in (NSArray *)(*value)) {
        if (![element isKindOfClass:NSString.class]) {
            return NO;
        }
    }
    return YES;
}

// MARK: cursorStyle

- (CursorStyle)cursorStyle {
    return [_defaults integerForKey:kPreferenceCursorStyleKey];
}

- (void)setCursorStyle:(CursorStyle)cursorStyle {
    [_defaults setInteger:cursorStyle forKey:kPreferenceCursorStyleKey];
}

- (BOOL)validateCursorStyle:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __CursorStyleFirst && _value < __CursorStyleLast;
}

- (NSString *)htermCursorShape {
    switch (self.cursorStyle) {
        case CursorStyleBlock:
            return @"BLOCK";
        case CursorStyleBeam:
            return @"BEAM";
        case CursorStyleUnderline:
            return @"UNDERLINE";
        default:
            NSAssert(NO, @"Invalid cursor style");
            return nil;
    }
}

// MARK: blinkCursor

- (BOOL)blinkCursor {
    return [_defaults boolForKey:kPreferenceBlinkCursorKey];
}

- (void)setBlinkCursor:(BOOL)blinkCursor {
    [_defaults setBool:blinkCursor forKey:kPreferenceBlinkCursorKey];
}

- (BOOL)validateBlinkCursor:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: hideStatusBar
- (BOOL)hideStatusBar {
    return [_defaults boolForKey:kPreferenceHideStatusBarKey];
}

- (void)setHideStatusBar:(BOOL)showStatusBar {
    [_defaults setBool:showStatusBar forKey:kPreferenceHideStatusBarKey];
}

- (BOOL)validateHideStatusBar:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSNumber.class];
}

// MARK: colorScheme
- (ColorScheme)colorScheme {
    return [_defaults integerForKey:kPreferenceColorSchemeKey];
}

- (void)setColorScheme:(ColorScheme)colorScheme {
    [_defaults setInteger:colorScheme forKey:kPreferenceColorSchemeKey];
}

- (BOOL)validateColorScheme:(id *)value error:(NSError **)error {
    if (![*value isKindOfClass:NSNumber.class]) {
        return NO;
    }
    int _value = [(NSNumber *)(*value) intValue];
    return _value >= __ColorSchemeFirst && _value < __ColorSchemeLast;
}

// MARK: hostnameOverride
- (NSString *)hostnameOverride {
    return [_defaults stringForKey:kHostnameOverrideKey];
}

- (void)setHostnameOverride:(NSString *)hostnameOverride {
    [_defaults setValue:hostnameOverride forKey:kHostnameOverrideKey];
}

- (BOOL)validateHostnameOverride:(id *)value error:(NSError **)error {
    return [*value isKindOfClass:NSString.class];
}

- (NSString *)_hostnameOverride {
    return _hostnameIsOverridden ? self.hostnameOverride : nil;
}

+ (BOOL)systemThemeIsDark {
    if (@available(iOS 12.0, *)) {
        switch (UIScreen.mainScreen.traitCollection.userInterfaceStyle) {
            case UIUserInterfaceStyleLight:
                return NO;
            case UIUserInterfaceStyleDark:
                return YES;
            default:
                break;
        }
    }
    return NO;
}

- (BOOL)requestingDarkAppearance {
    return (self.class.systemThemeIsDark && !self.theme.appearance.darkOverride) || (!self.class.systemThemeIsDark && self.theme.appearance.lightOverride);
}

- (UIUserInterfaceStyle)userInterfaceStyle {
    return self.requestingDarkAppearance ? UIUserInterfaceStyleDark : UIUserInterfaceStyleLight;
}

- (UIKeyboardAppearance)keyboardAppearance {
    return self.requestingDarkAppearance ? UIKeyboardAppearanceDark : UIKeyboardAppearanceLight;
}

- (UIStatusBarStyle)statusBarStyle {
    return self.requestingDarkAppearance ? UIStatusBarStyleLightContent : UIStatusBarStyleDefault;
}

@end
