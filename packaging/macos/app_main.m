#import <AppKit/AppKit.h>
#include <unistd.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *resources = [[NSBundle mainBundle] resourcePath];
        NSString *port = [resources stringByAppendingPathComponent:@"port"];
        NSString *python = [resources stringByAppendingPathComponent:@"python/bin/python3"];
        NSString *launcher = [port stringByAppendingPathComponent:@"launcher/bbport_launcher_mac.py"];
        NSString *deps = [port stringByAppendingPathComponent:@"deps"];
        setenv("BB_PACKAGED", "1", 1);
        setenv("BB_PREBUILT", "1", 1);
        setenv("BB_DEPS_PREFIX", [deps fileSystemRepresentation], 1);
        unsetenv("BB_PROBE");
        unsetenv("DYLD_LIBRARY_PATH");
        unsetenv("DYLD_FALLBACK_LIBRARY_PATH");
        char **arguments = calloc((size_t)argc + 2, sizeof(char *));
        if (arguments) {
            arguments[0] = (char *)[python fileSystemRepresentation];
            arguments[1] = (char *)[launcher fileSystemRepresentation];
            for (int i = 1; i < argc; ++i) arguments[i + 1] = argv[i];
            execv(arguments[0], arguments);
        }
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Bloodborne launcher could not start"];
        [alert setInformativeText:@"The bundled native Python runtime is missing or could not execute. Reinstall the app."];
        [alert runModal];
        return 1;
    }
}
