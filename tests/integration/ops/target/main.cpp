struct V { int x, y, z; };
int g;

int Sum(V* v, int k)
{
    int r = k * v->z + v->x;
    if (r > 10)
    {
        g = r;
    }
    else
    {
        g = v->y;
    }
    return r + v->y;
}
