\! gs_ktool -d all
\! gs_ktool -g

CREATE CLIENT MASTER KEY MyCMK WITH ( KEY_STORE = gs_ktool , KEY_PATH = "gs_ktool/1" , ALGORITHM = AES_256_CBC);
CREATE COLUMN ENCRYPTION KEY MyCEK770 WITH VALUES (CLIENT_MASTER_KEY = MyCMK, ALGORITHM = AEAD_AES_256_CBC_HMAC_SHA256);
SELECT classid::regclass, column_key_name, refclassid::regclass, global_key_name FROM pg_depend JOIN gs_client_global_keys on refobjid = gs_client_global_keys.Oid JOIN gs_column_keys ON objid=gs_column_keys.Oid WHERE classid = 9720;
CREATE TABLE IF NOT EXISTS tr2(i1 INT ENCRYPTED WITH (COLUMN_ENCRYPTION_KEY = MyCEK770, ENCRYPTION_TYPE = DETERMINISTIC) , i2 INT);
DROP TABLE tr2;
DROP COLUMN ENCRYPTION KEY MyCEK770;
DROP  CLIENT MASTER KEY MyCMK;
SELECT classid::regclass, column_key_name, refclassid::regclass, global_key_name FROM pg_depend JOIN gs_client_global_keys on refobjid = gs_client_global_keys.Oid JOIN gs_column_keys ON objid=gs_column_keys.Oid WHERE classid = 9720;

\! gs_ktool -d all