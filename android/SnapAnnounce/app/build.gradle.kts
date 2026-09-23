plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

/*
 * Release signing comes from the environment (the CI workflow decodes the
 * keystore from a secret), so no key or password ever sits in the repo.
 * Without it, assembleRelease still builds, just unsigned; debug builds
 * are not affected either way.
 */
val releaseKeystore: String? = System.getenv("SNAPANNOUNCE_KEYSTORE_FILE")

android {
    namespace = "com.pillepalle.snapannounce"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.pillepalle.snapannounce"
        // AudioRecord/foreground-service APIs used here are stable well
        // before this; 26 also buys NotificationChannel for free.
        minSdk = 26
        targetSdk = 34
        // Set from the release tag by CI: -PappVersionName=0.2.0 -PappVersionCode=<run>.
        versionCode = (project.findProperty("appVersionCode") as String?)?.toInt() ?: 1
        versionName = project.findProperty("appVersionName") as String? ?: "0.1"
    }

    signingConfigs {
        if (releaseKeystore != null) {
            create("release") {
                storeFile = file(releaseKeystore)
                storePassword = System.getenv("SNAPANNOUNCE_STORE_PASSWORD")
                keyAlias = System.getenv("SNAPANNOUNCE_KEY_ALIAS")
                keyPassword = System.getenv("SNAPANNOUNCE_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (releaseKeystore != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.06.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.2")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.2")
    implementation("androidx.lifecycle:lifecycle-service:2.8.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
