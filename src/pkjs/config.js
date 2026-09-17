// Watchapp settings page
module.exports = [
  {
    type: 'heading',
    defaultValue: 'Radiant Lyrics',
  },
  {
    type: 'text',
    defaultValue: 'Synced TIDAL lyrics from the RL Manager Pebble Watch integration.',
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Lyrics' },
      {
        type: 'radiogroup',
        messageKey: 'SetMode',
        label: 'Sync level',
        defaultValue: '2',
        options: [
          { label: 'Line', value: '0' },
          { label: 'Word', value: '1' },
          { label: 'Syllable', value: '2' },
        ],
      },
      {
        type: 'toggle',
        messageKey: 'SetBgVocals',
        label: 'Background vocals',
        description: 'Show adlibs & background vocals under the main line',
        defaultValue: true,
      },
      {
        type: 'toggle',
        messageKey: 'SetUpcoming',
        label: 'Upcoming lines',
        description: 'Show the upcoming lines under the current one',
        defaultValue: true,
      },
      {
        type: 'slider',
        messageKey: 'SetSyncOffset',
        label: 'Sync offset (ms)',
        description: 'Shift lyrics earlier (-) or later (+)',
        defaultValue: 0,
        min: -1000,
        max: 1000,
        step: 50,
      },
    ],
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Look' },
      {
        type: 'radiogroup',
        messageKey: 'SetStyle',
        label: 'Style',
        defaultValue: '0',
        options: [
          { label: 'Normal (left aligned, duet singers on the right)', value: '0' },
          { label: 'Centered', value: '1' },
        ],
      },
      {
        type: 'select',
        messageKey: 'SetTextSize',
        label: 'Text size',
        defaultValue: '2',
        options: [
          { label: 'Small', value: '0' },
          { label: 'Medium', value: '1' },
          { label: 'Large', value: '2' },
        ],
      },
      {
        type: 'color',
        messageKey: 'SetAccent',
        label: 'Highlight colour',
        defaultValue: 'FFFFFF',
        sunlight: false,
        capabilities: ['COLOR'],
      },
      {
        type: 'color',
        messageKey: 'SetBgColor',
        label: 'Background colour',
        defaultValue: '000000',
        sunlight: false,
        capabilities: ['COLOR'],
      },
      {
        type: 'toggle',
        messageKey: 'SetProgress',
        label: 'Progress bar',
        defaultValue: true,
      },
    ],
  },
  {
    type: 'section',
    items: [
      { type: 'heading', defaultValue: 'Behaviour' },
      {
        type: 'toggle',
        messageKey: 'SetAutoOpen',
        label: 'Open automatically',
        description: 'Open when TIDAL has timed lyrics and close again (back to your watchface) when a track has none',
        defaultValue: true,
      },
      {
        type: 'select',
        messageKey: 'SetBacklight',
        label: 'Backlight',
        description: 'Tap Toggle: tap the screen once to turn the backlight on, tap again to turn it off. Double tap still does the normal short backlight (watches without a touchscreen use the wrist gesture). Always On and Tap Toggle use more battery.',
        defaultValue: '0',
        options: [
          { label: 'Default', value: '0' },
          { label: 'Always On', value: '1' },
          { label: 'Tap Toggle', value: '2' },
        ],
      },
    ],
  },
  {
    type: 'submit',
    defaultValue: 'Save',
  },
];
