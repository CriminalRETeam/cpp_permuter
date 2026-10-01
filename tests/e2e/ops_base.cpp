struct V { int x, y, z; };
int g;

int Sum(V* v, int k)
{
    int r = v->x + k * v->z;
    if (r <= 10)
    {
        g = v->y;
    }
    else
    {
        g = r;
    }
    return r + v->y;
}
