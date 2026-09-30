plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.igg.lordsm.panel"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.igg.lordsm.panel"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "lordsM"

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=none"
                cppFlags += ""
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        create("release") {
            // Filled from secrets at CI time; falls back to the debug key so
            // a local build still produces an installable APK.
            val ks = System.getenv("BOT_KEYSTORE")
            if (ks != null) {
                storeFile = file(ks)
                storePassword = System.getenv("BOT_KEYSTORE_PASS")
                keyAlias = System.getenv("BOT_KEY_ALIAS")
                keyPassword = System.getenv("BOT_KEY_PASS")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = if (System.getenv("BOT_KEYSTORE") != null)
                signingConfigs.getByName("release") else signingConfigs.getByName("debug")
        }
        debug {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    // Deliberately empty. The UI is framework-only (no AndroidX, no
    // Compose) so the APK can be produced by build.sh on any machine with
    // a JDK, d8 and aapt. Nothing here needs to resolve.
}
