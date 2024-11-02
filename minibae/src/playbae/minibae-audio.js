import PlayBAE from './playbae.js';

const TYPE_TO_CMD = {
    'wav': '-w',
    'aif': '-a',
    'rmf': '-r',
    'mid': '-m'
};

// Function to initialize and resume the AudioContext
function initAndResumeAudioContext() {
    const audioContext = new AudioContext();

    // Function to resume AudioContext
    const resumeAudioContext = () => {
        if (audioContext.state === 'suspended') {
            audioContext.resume().then(() => {
                console.log('AudioContext resumed');
            }).catch((err) => {
                console.error('Failed to resume AudioContext:', err);
            });
        }
    };

    // Attach the resume function to user interaction events
    document.addEventListener('click', resumeAudioContext, { once: true });
    document.addEventListener('keydown', resumeAudioContext, { once: true });
    document.addEventListener('touchstart', resumeAudioContext, { once: true });

    return audioContext;
}


class AudioBufferManager {
    constructor(audioContext, sampleRate, bufferThreshold = 8192) {
        this.audioContext = audioContext;
        this.sampleRate = sampleRate;
        this.bufferThreshold = bufferThreshold;
        this.leftChannelBuffer = [];
        this.rightChannelBuffer = [];
        this.scheduledTime = this.audioContext.currentTime;
    }

    addAudioData(int16Data) {
        // Deinterleave and convert Int16 to Float32 for each chunk
        for (let i = 0; i < int16Data.length; i += 2) {
            this.leftChannelBuffer.push(int16Data[i] / 32768);
            this.rightChannelBuffer.push(int16Data[i + 1] / 32768);
        }

        // If we have enough samples, play the audio
        if (this.leftChannelBuffer.length >= this.bufferThreshold) {
            this.playBufferedAudio();
        }
    }

    playBufferedAudio() {
        const length = this.bufferThreshold;
        const audioBuffer = this.audioContext.createBuffer(2, length, this.sampleRate);

        // Prepare Float32Arrays for the AudioBuffer
        const leftChannel = new Float32Array(this.leftChannelBuffer.slice(0, length));
        const rightChannel = new Float32Array(this.rightChannelBuffer.slice(0, length));

        // Remove the used samples from the buffer
        this.leftChannelBuffer = this.leftChannelBuffer.slice(length);
        this.rightChannelBuffer = this.rightChannelBuffer.slice(length);

        // Copy the data to the AudioBuffer channels
        audioBuffer.copyToChannel(leftChannel, 0);
        audioBuffer.copyToChannel(rightChannel, 1);

        // Create a new source node for each playback
        const sourceNode = this.audioContext.createBufferSource();
        sourceNode.buffer = audioBuffer;
        sourceNode.connect(this.audioContext.destination);

        // Schedule playback slightly in the future to prevent gaps
        if (this.scheduledTime < this.audioContext.currentTime) {
            this.scheduledTime = this.audioContext.currentTime + 0.05; // Add a small buffer time
        }
        sourceNode.start(this.scheduledTime);

        // Update the scheduled time for the next buffer
        this.scheduledTime += audioBuffer.duration;
    }
}

class MiniBAEAudio extends HTMLAudioElement {
    constructor() {
        super();
        window.miniBAEInstance = this;
        this.initBAE();       
        this.audioContext = new initAndResumeAudioContext();
        this.sampleRate = 44100; // Set the sample rate for your audio
        this.bufferManager = new AudioBufferManager(this.audioContext, this.sampleRate);
    }

    async playAudioData(leftChannel, rightChannel) {
        

        // Create a new AudioBuffer with 2 channels (stereo), matching sample rate and data length
        const audioBuffer = audioContext.createBuffer(2, leftChannel.length, audioContext.sampleRate);
    
        // Copy the left and right channels into the AudioBuffer
        audioBuffer.copyToChannel(leftChannel, 0); // Left channel
        audioBuffer.copyToChannel(rightChannel, 1); // Right channel
    
        // Create a new AudioBufferSourceNode
        const sourceNode = audioContext.createBufferSource();
        sourceNode.buffer = audioBuffer;
    
        // Connect the source node to the audio context's destination
        sourceNode.connect(audioContext.destination);
    
        // Start playback
        sourceNode.start();  // This should now work if `sourceNode` is valid
        console.log("AudioContext state:", audioContext.state);
        console.log("AudioBuffer channels:", audioBuffer.numberOfChannels);
        console.log("AudioBuffer sampleRate:", audioBuffer.sampleRate);
    }

    async initBAE() {
        let playbaeOptions = {};
        playbaeOptions.arguments = ['-v', '255', '-t', '1200'];

        playbaeOptions.preRun = [(Module) => {
            this.postAudioData  = (pointer, length) => {
                const int16Data = new Int16Array(Module.HEAP16.buffer, pointer, length * 2); // Int16 data is half the byte length
                this.bufferManager.addAudioData(int16Data);
            };
            
            Module.FS.createPreloadedFile("/home/web_user/", "audio", this.currentSrc, true, true);
            if(this.dataset.minibaePatches) {
                playbaeOptions.arguments.push("-p", "/home/web_user/patches_custom.hsb");
                Module.FS.createPreloadedFile("/home/web_user/", "patches_custom.hsb", this.dataset.minibaePatches, true, true);
            } else {
                playbaeOptions.arguments.push("-p", "/home/web_user/patches.hsb");
            }
        }];

        if(this.dataset.minibaeType) {
            playbaeOptions.arguments.push(TYPE_TO_CMD[this.dataset.minibaeType]);
        } else {
            playbaeOptions.arguments.push('-f');
        }
        playbaeOptions.arguments.push("/home/web_user/audio");
        playbaeOptions.arguments.push("-o", "null");
        
        let playbae = await PlayBAE(playbaeOptions);
    }
}

customElements.define('minibae-audio', MiniBAEAudio, {extends: 'audio'});