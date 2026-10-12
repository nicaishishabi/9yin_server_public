-- migrate:statement
CREATE PROCEDURE nineyin_m2_preflight()
BEGIN
  IF (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('accounts','roles')) <> 2 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 preflight: legacy accounts/roles tables are required';
  END IF;
  IF (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name IN ('accounts_m2','roles_m2','role_appearances_m2','role_locations_m2','accounts_legacy_v1','roles_legacy_v1')) <> 0 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 preflight: staging or legacy-v1 tables already exist';
  END IF;
  IF (SELECT COUNT(*) FROM roles r LEFT JOIN accounts a ON a.account_key=r.account_key WHERE a.account_key IS NULL) <> 0 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 preflight: orphan legacy roles';
  END IF;
  IF (SELECT COUNT(*) FROM roles WHERE JSON_VALID(appearance)=0) <> 0 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 preflight: invalid appearance JSON';
  END IF;
  IF (SELECT COUNT(*) FROM (SELECT account_key FROM roles GROUP BY account_key HAVING COUNT(*) > 1) duplicate_accounts) <> 0 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 preflight: multiple legacy roles for one account';
  END IF;
  IF (SELECT COUNT(*) FROM (SELECT name FROM roles GROUP BY name HAVING COUNT(*) > 1) duplicate_names) <> 0 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 preflight: duplicate legacy role names';
  END IF;
END

-- migrate:statement
CALL nineyin_m2_preflight()

-- migrate:statement
DROP PROCEDURE nineyin_m2_preflight

-- migrate:statement
CREATE TABLE accounts_m2 (
  account_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  account_key VARBINARY(128) NOT NULL,
  password_hash VARBINARY(255) NULL,
  status TINYINT UNSIGNED NOT NULL DEFAULT 1,
  version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (account_id),
  UNIQUE KEY uq_accounts_key (account_key),
  CONSTRAINT ck_accounts_status CHECK (status IN (0,1,2))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci

-- migrate:statement
CREATE TABLE roles_m2 (
  role_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  account_id BIGINT UNSIGNED NOT NULL,
  slot SMALLINT UNSIGNED NOT NULL,
  name VARCHAR(64) NOT NULL,
  delete_requested_at TIMESTAMP(6) NULL,
  version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (role_id),
  UNIQUE KEY uq_roles_account_slot (account_id, slot),
  UNIQUE KEY uq_roles_name (name),
  KEY ix_roles_account (account_id),
  CONSTRAINT fk_m2_roles_account FOREIGN KEY (account_id) REFERENCES accounts_m2(account_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci

-- migrate:statement
CREATE TABLE role_appearances_m2 (
  role_id BIGINT UNSIGNED NOT NULL,
  format_version SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  appearance JSON NOT NULL,
  version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (role_id),
  CONSTRAINT fk_m2_appearance_role FOREIGN KEY (role_id) REFERENCES roles_m2(role_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci

-- migrate:statement
CREATE TABLE role_locations_m2 (
  role_id BIGINT UNSIGNED NOT NULL,
  scene_config VARCHAR(255) NOT NULL,
  scene_resource VARCHAR(64) NOT NULL,
  pos_x FLOAT NOT NULL,
  pos_y FLOAT NOT NULL,
  pos_z FLOAT NOT NULL,
  orient FLOAT NOT NULL,
  version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (role_id),
  KEY ix_role_locations_scene (scene_resource),
  CONSTRAINT fk_m2_location_role FOREIGN KEY (role_id) REFERENCES roles_m2(role_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci

-- migrate:statement
INSERT INTO accounts_m2(account_key, created_at, updated_at)
SELECT CAST(account_key AS BINARY), created_at, updated_at FROM accounts

-- migrate:statement
INSERT INTO roles_m2(role_id, account_id, slot, name, created_at, updated_at)
SELECT r.id, a2.account_id, 0, r.name, r.created_at, r.updated_at
FROM roles r JOIN accounts_m2 a2 ON a2.account_key=CAST(r.account_key AS BINARY)

-- migrate:statement
INSERT INTO role_appearances_m2(role_id, appearance, updated_at)
SELECT id, appearance, updated_at FROM roles

-- migrate:statement
INSERT INTO role_locations_m2(role_id, scene_config, scene_resource, pos_x, pos_y, pos_z, orient, updated_at)
SELECT id, scene_config, scene_resource, pos_x, pos_y, pos_z, orient, updated_at FROM roles

-- migrate:statement
CREATE PROCEDURE nineyin_m2_verify_copy()
BEGIN
  IF (SELECT COUNT(*) FROM accounts_m2) <> (SELECT COUNT(*) FROM accounts) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 verify: account row count differs';
  END IF;
  IF (SELECT COUNT(*) FROM roles_m2) <> (SELECT COUNT(*) FROM roles) OR
     (SELECT COUNT(*) FROM role_appearances_m2) <> (SELECT COUNT(*) FROM roles) OR
     (SELECT COUNT(*) FROM role_locations_m2) <> (SELECT COUNT(*) FROM roles) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 verify: role child row count differs';
  END IF;
  IF (SELECT COUNT(*) FROM roles old
      JOIN accounts_m2 a2 ON a2.account_key=CAST(old.account_key AS BINARY)
      JOIN roles_m2 r2 ON r2.role_id=old.id
      JOIN role_appearances_m2 ap2 ON ap2.role_id=old.id
      JOIN role_locations_m2 l2 ON l2.role_id=old.id
      WHERE r2.account_id<>a2.account_id OR r2.slot<>0 OR BINARY r2.name<>BINARY old.name
         OR CAST(ap2.appearance AS CHAR CHARACTER SET utf8mb4)<>CAST(old.appearance AS CHAR CHARACTER SET utf8mb4)
         OR BINARY l2.scene_config<>BINARY old.scene_config OR BINARY l2.scene_resource<>BINARY old.scene_resource
         OR l2.pos_x<>old.pos_x OR l2.pos_y<>old.pos_y OR l2.pos_z<>old.pos_z OR l2.orient<>old.orient) <> 0 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'M2 verify: copied role fields differ';
  END IF;
END

-- migrate:statement
CALL nineyin_m2_verify_copy()

-- migrate:statement
DROP PROCEDURE nineyin_m2_verify_copy

-- migrate:statement
RENAME TABLE accounts TO accounts_legacy_v1,
             roles TO roles_legacy_v1,
             accounts_m2 TO accounts,
             roles_m2 TO roles,
             role_appearances_m2 TO role_appearances,
             role_locations_m2 TO role_locations
