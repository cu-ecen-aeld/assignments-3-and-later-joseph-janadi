#include <stdio.h>
#include <syslog.h>
#include <string.h>

int main(int argc, char *argv[])
{
    // Open syslog connection
    openlog(NULL, 0, LOG_USER);

    // Check for correct number of args
    if (argc != 3) {
        syslog(LOG_ERR, "Two arguments required: file path and write string");
        closelog();
        return 1;
    }
    char *writefile = argv[1];
    char *writestr = argv[2];

    // Open file and write to it
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);
    FILE *fp = fopen(writefile, "w");
    if (!fp) {
        syslog(LOG_ERR, "Unable to open %s", writefile);
        closelog();
        return 1;
    }

    size_t num_bytes_written = fwrite(writestr, 1, strlen(writestr), fp);
    if (num_bytes_written != strlen(writestr)) {
        syslog(LOG_ERR, "Exact string was not written to file");
        fclose(fp);
        closelog();
        return 1;
    }

    fclose(fp);
    closelog();

    return 0;
}
