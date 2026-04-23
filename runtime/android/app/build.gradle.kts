// J2ME runtime — Android app module.
//
// Build steps (one-time):
//   1. Install Android SDK + NDK 26.x (or newer). Record paths in
//      local.properties (copy local.properties.example).
//   2. Run  ../fetch_sdl.sh  from this dir to drop SDL2 + satellites
//      into app/jni/SDL, SDL_image, SDL_ttf, SDL_mixer.
//   3. Drop a MIDlet JAR at  app/src/main/assets/game.jar  and edit
//      app/src/main/assets/args.cfg to name its MIDlet class.
//   4. cd runtime/android && ./gradlew assembleDebug  → APK at
//      app/build/outputs/apk/debug/app-debug.apk
//
// First build pulls Gradle (~150 MB) + SDL source + CMake toolchain.
// Subsequent builds are ~seconds for incremental NDK compiles.

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.j2me.runtime"
    compileSdk = 35
    // NDK 27+ ships libc++_shared.so with 16 KB-aligned LOAD segments,
    // which Android 15 on ARM devices now warns about (and will enforce
    // in a later release). 26.x's libc++_shared is 4 KB aligned.
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "com.j2me.runtime"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"

        externalNativeBuild {
            cmake {
                // SDL's build system uses GNU extensions in headers.
                cppFlags += listOf("-std=c++20", "-fexceptions", "-frtti")
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_ARM_NEON=TRUE",
                )
                // 16 KB page alignment for Android 15+ compatibility. AGP
                // also propagates this to SDL2/image/ttf/mixer .so files
                // built via the same CMake invocation.
                cFlags   += listOf("-Wl,-z,max-page-size=16384")
                cppFlags += listOf("-Wl,-z,max-page-size=16384")
            }
        }
        ndk {
            // 64-bit only by default — arm64-v8a covers every Play-Store-eligible
            // device. Add "armeabi-v7a", "x86_64" here to widen coverage.
            abiFilters += listOf("arm64-v8a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("jni/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
        debug {
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    // Prefab lets SDL consume AAR deps; left here in case you want to swap
    // the vendored sources for Maven-published SDL AARs later.
    buildFeatures {
        prefab = false
    }

    packaging {
        // Don't compress the JAR — avoids re-parse cost at runtime when we
        // extract it to internal storage.
        resources.excludes += setOf("META-INF/DEPENDENCIES")
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
}
