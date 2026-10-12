pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // RadialGamePad, the on-screen controls, is published there only.
        maven("https://jitpack.io") {
            content { includeGroup("com.github.swordfish90") }
        }
    }
}

rootProject.name = "OpenCMR2"
include(":app")
