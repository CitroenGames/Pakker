# Pakker

Pakker is an asset resource storer designed to efficiently manage and store multiple files into a single archive. 
This tool is useful for games, applications, or any projects that require bundling various resources like images, sounds, scripts, or other data files.

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

## Loading LUA script example

```cpp
#include "Pak.h"
#include <iostream>
#include <lua.hpp>

int main() {
    Pakker pakker;

    // Load Lua file from PAK
    std::shared_ptr<std::vector<uint8_t>> luaFileData = pakker.LoadFile("example.pak", "script.lua");

    if (luaFileData) {
        std::string luaScript(luaFileData->begin(), luaFileData->end());

        // Initialize Lua state
        lua_State *L = luaL_newstate();
        luaL_openlibs(L);

        // Load and execute the Lua script
        if (luaL_dostring(L, luaScript.c_str()) != LUA_OK) {
            std::cerr << "Error executing Lua script: " << lua_tostring(L, -1) << std::endl;
        }

        // Close the Lua state
        lua_close(L);
    } else {
        std::cerr << "Failed to load the Lua file from PAK" << std::endl;
    }

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

#### Creating a PAK file

To create a PAK file from a directory of files:
```cpp
#include "Pak.h"

Pakker pakker;
pakker.CreatePakFromFolder("output.pak", "path/to/your/folder");
```

#### Extracting a PAK file

To extract all files from a PAK file:
```cpp
#include "Pak.h"

Pakker pakker;
pakker.ExtractPak("output.pak", "output/directory");
```

#### Listing Contents of a PAK file

To list all files in a PAK file:
```cpp
#include "Pak.h"

Pakker pakker;
pakker.ListPak("output.pak");
```

#### Reading a Specific File from a PAK file

To read a specific file from a PAK file:
```cpp
#include "Pak.h"
#include <iostream>

Pakker pakker;
auto fileData = pakker.LoadFile("output.pak", "filename.ext");

if (fileData) {
    std::cout << "File loaded successfully!" << std::endl;
} else {
    std::cerr << "Failed to load the file." << std::endl;
}
```
## Contributing
Contributions are welcome! Please feel free to submit a Pull Request.
## License
This project is licensed under the MIT License - see the LICENSE file for details.
