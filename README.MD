# 3D Game Asset Manager

This project demonstrates a comprehensive asset management system for 3D games, incorporating model loading, audio playback, and a custom file packaging system.

## Features

- Custom PAK file system for efficient asset storage and retrieval
- 3D model loading using Assimp
- Audio playback using miniaudio
- Easy integration with game engines and rendering systems

## Dependencies

- C++17 compatible compiler

## PAK File System

Our custom PAK file system provides efficient storage and retrieval of game assets:

- Create PAK files to bundle multiple assets
- Extract assets from PAK files
- List contents of PAK files
- Add new assets to existing PAK files
- Simple encryption for basic asset protection

## 3D Model Loading

Using Assimp, we can load various 3D model formats:

```cpp
std::shared_ptr<aiScene> load3DModel(const std::vector<uint8_t>& data, const std::string& filename) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFileFromMemory(
        data.data(),
        data.size(),
        aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenNormals,
        filename.c_str()
    );
    // ... error checking and scene handling
}
```

## Audio Playback
Integrate miniaudio for high-performance audio playback:

```cpp
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

void audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    // Audio processing logic here
}

int main() {
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate        = 44100;
    deviceConfig.dataCallback      = audio_callback;

    ma_device device;
    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
        return -1;
    }

    ma_device_start(&device);
    // Main game loop
    ma_device_uninit(&device);
    return 0;
}
```

## Usage Example

```cpp
int main() {
    PakFile pakFile;
    
    // Create a PAK file with 3D models and audio
    std::map<std::string, std::vector<uint8_t>> files;
    // ... (load your 3D model and audio files into the 'files' map)
    pakFile.createPak("assets.pak", files);
    
    // Load a 3D model from the PAK file
    std::shared_ptr<std::vector<uint8_t>> modelData = pakFile.loadFile("assets.pak", "model.fbx");
    if (modelData) {
        std::shared_ptr<aiScene> scene = load3DModel(*modelData, "model.fbx");
        if (scene) {
            // Use the imported scene for rendering
        }
    }
    
    // Load and play audio from the PAK file
    std::shared_ptr<std::vector<uint8_t>> audioData = pakFile.loadFile("assets.pak", "sound.wav");
    if (audioData) {
        // Use miniaudio to play the audio data
    }
    
    // Add a new asset to the existing PAK file
    std::vector<uint8_t> newAssetData;
    // ... (load new asset data)
    pakFile.addFileToPak("assets.pak", "new_asset.gltf", newAssetData);
    
    return 0;
}
```

## Contributing
Contributions are welcome! Please feel free to submit a Pull Request.
## License
This project is licensed under the MIT License - see the LICENSE file for details.