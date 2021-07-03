# Project file format
Projects are stored in a JSON file of the following format:
```json
{
	"version":"1.0",
	"input":{
		"path":"<absolute path>",
		/* optionally more, e.g., md5/sha1 for consistency */
	},
	"slices":[
		{
			"begin":<frame_begin>,
			"end":<frame_end>
		},
		/* more slices */
	]
}
```
