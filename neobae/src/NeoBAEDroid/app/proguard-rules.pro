# NeoBAE JNI bridge — native symbols are Java_com_zefie_NeoBAE_*
# (includes _makeCurrent / export natives; do not shrink or rename this package)
-keep class com.zefie.NeoBAE.** { *; }

# LoadResult fields are written via GetFieldID in native loadFromMemory
-keepclassmembers class com.zefie.NeoBAE.LoadResult {
    public int type;
    public int fileType;
    public int result;
    public long songReference;
    public long soundReference;
}

# MetaEventListener.onMetaEvent looked up via GetMethodID("(I[B)V")
# Keep Kotlin inner implementors (KaraokeHandler) and the method name.
-keep interface com.zefie.NeoBAE.Song$MetaEventListener { *; }
-keep class * implements com.zefie.NeoBAE.Song$MetaEventListener { *; }
-keepclassmembers class * implements com.zefie.NeoBAE.Song$MetaEventListener {
    <init>(...);
    void onMetaEvent(int, byte[]);
}
-keepattributes InnerClasses,EnclosingMethod,Signature

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
