# NeoBAE JNI bridge — native symbols are Java_com_zefie_NeoBAE_*
-keep class com.zefie.NeoBAE.Mixer { *; }
-keep class com.zefie.NeoBAE.Song { *; }
-keep class com.zefie.NeoBAE.Sound { *; }

# LoadResult fields are written via GetFieldID in native loadFromMemory
-keep class com.zefie.NeoBAE.LoadResult { *; }

# MetaEventListener.onMetaEvent looked up via GetMethodID("(I[B)V")
-keep interface com.zefie.NeoBAE.Song$MetaEventListener { *; }
-keep class * implements com.zefie.NeoBAE.Song$MetaEventListener { *; }
-keepclassmembers class * implements com.zefie.NeoBAE.Song$MetaEventListener {
    void onMetaEvent(int, byte[]);
}

# Kotlin SQLite JNI (Java_com_zefie_NeoBAEDroid_database_SQLiteHelper_*)
-keep class com.zefie.NeoBAEDroid.database.SQLiteHelper { *; }

# Keep all native method names/descriptors app-wide
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# SharedPreferences persists SortMode by enum name
-keepclassmembers enum com.zefie.NeoBAEDroid.SortMode { *; }
-keepclassmembers enum com.zefie.NeoBAEDroid.RepeatMode { *; }
-keepclassmembers enum com.zefie.NeoBAEDroid.NavigationScreen { *; }
