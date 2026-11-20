// zeebo_movie.h - MPEG-1 movie playback (video via GLES1 RGB565 + audio via BREW IMedia).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts playing an MPEG-1 movie file (e.g. "movies/Logo.mpg"). Returns 1 on success, 0 on failure.
int  Zeebo_MovieStart(const char* path);

// Advances movie playback by one frame and renders it to the current GLES1 surface.
// Returns 1 while still playing, 0 when finished or stopped.
int  Zeebo_MovieTick(void);

// Stops playback and frees all video/audio/GL resources.
void Zeebo_MovieStop(void);

// Requests immediate skip (e.g. when the user presses a button/key).
void Zeebo_MovieSkip(void);

// Returns 1 if a movie is currently playing.
int  Zeebo_MovieIsPlaying(void);

#ifdef __cplusplus
}
#endif
