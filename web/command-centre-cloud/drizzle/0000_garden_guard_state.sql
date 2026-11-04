CREATE TABLE `garden_guard_state` (
  `drone_id` text PRIMARY KEY NOT NULL,
  `revision` integer NOT NULL,
  `instance_id` text NOT NULL,
  `synced_at` real NOT NULL,
  `state_json` text NOT NULL
);
